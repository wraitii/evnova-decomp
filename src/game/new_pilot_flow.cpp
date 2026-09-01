#include "new_pilot_flow.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "hud_overlay.hpp"
#include "intro_cinematic.hpp"
#include "mission.hpp"
#include "nova_font.hpp"
#include "outfit.hpp"
#include "pilot_file.hpp"
#include "ship_spawn.hpp"
#include "travel.hpp"
#include "ui_dialog.hpp"
#include "weapon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace game {
namespace {

// Where a brand-new pilot begins. The real game randomizes the start system
// among a few candidates at PilotData_InitializePlayerState (fresh-seed picks a
// random valid starting system from the pilot-block's four stored choices); we
// hardcode one (Tichel, zero-based 1) for now so the in-flight view has a known
// landmark. TODO(decomp): implement the randomized start-system selection once
// the pilot-block start candidates are decoded.
constexpr std::int16_t kStartSystemId = 1;            // zero-based (Tichel)
constexpr std::int16_t kStartSystemResourceId = 0x81; // resource id

// NovaText_StripLeadingArticle: drops the leading article from a name string.
// TODO(decomp): the original's exact article set is not reconstructed; the
// stock STR# 0x80 sample names carry none, so this only guards mod-added
// entries (provisional).
void StripLeadingArticle(std::string &name) {
  constexpr std::string_view kThe = "the ";
  if (name.size() > kThe.size() &&
      std::equal(kThe.begin(), kThe.end(), name.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
      })) {
    name.erase(0, kThe.size());
  }
}

// NameString_StripSubtitleSuffix: drops a trailing "the <x>"-style suffix.
// TODO(decomp): suffix marker not reconstructed; provisional no-op that keeps
// the call-site ordering visible.
void StripSubtitleSuffix(std::string &) {}

[[nodiscard]] int RandomIndex(GameState &state, int count) {
  return static_cast<int>(
      std::uniform_int_distribution<int>{0, count - 1}(state.rng));
}

// Picks the stellar resource id the player should spawn near in a system: the
// first owned body (running the system's nav_defs). This stands in for the
// original's landing/launch placement (which positions the ship beside the
// planet it last docked at); the jump-gate vs planet choice is not yet
// distinguished, so we take the system's first owned body.
[[nodiscard]] std::int16_t
PickLandingStellarResource(std::span<const std::int16_t> nav_defs) {
  for (const auto nav : nav_defs) {
    if (nav >= 0x80) {
      return nav;
    }
  }
  return -1;
}

// ===========================================================================
// Dialog ports (run on the SDL-backed dialog runtime in ui_dialog.cpp)
// ---------------------------------------------------------------------------

// The 0x63688a72 family census Menu_RunPilotSelectionDialog performs: count
// the family's non-hidden entries (metadata name not starting with '.'). The
// port enumerates the ch\x9ar resources loaded from the archives; the original
// also merges the in-memory pilot blocks created during a session.
// TODO(decomp): in-memory registry merge not reconstructed.
struct PilotTemplateEntry {
  std::uint16_t resource_id = 0;
  std::string name;
  bool hidden = false;
};

[[nodiscard]] std::vector<PilotTemplateEntry> EnumeratePilotTemplates() {
  std::vector<PilotTemplateEntry> out;
  for (const auto &[type_code, id] : NovaResource_AllKeys()) {
    if (type_code != kResourceTypeCharacter) {
      continue;
    }
    if (std::any_of(out.begin(), out.end(), [&](const PilotTemplateEntry &e) {
          return e.resource_id == id;
        })) {
      continue;
    }
    auto named = NovaResource_LoadNamed(kResourceTypeCharacter, id);
    if (!named) {
      continue;
    }
    PilotTemplateEntry entry;
    entry.resource_id = id;
    entry.name = named->name;
    entry.hidden = !named->name.empty() && named->name[0] == '.';
    out.push_back(std::move(entry));
  }
  return out;
}

// Ghidra 0x004cd350 PilotData_ResolveStartType: the starting ship class from
// the named character block (+4 minus 0x80; values < 0x80 resolve to class 0).
[[nodiscard]] std::int16_t
ResolveStartTypeFromTemplate(const std::string &template_name) {
  for (const auto &entry : EnumeratePilotTemplates()) {
    if (entry.name != template_name) {
      continue;
    }
    const auto block =
        NovaResource_Load(kResourceTypeCharacter, entry.resource_id);
    if (!block || block->size() < 6) {
      return 0;
    }
    const std::int16_t designator = static_cast<std::int16_t>(
        (std::to_integer<unsigned>((*block)[4]) << 8U) |
        std::to_integer<unsigned>((*block)[5]));
    return designator >= 0x80 ? static_cast<std::int16_t>(designator - 0x80)
                              : 0;
  }
  return 0;
}

// Ghidra 0x0048a7e0 Menu_RunPilotSelectionDialog. DITL rows (1-based):
// 4 = Strict Play checkbox (code 4), 8/9 = Full Name / Nickname edit texts
// (prefilled from STR# 0x80 rows 1-3 / 4-6), 11 = Gender popup (MENU 0x1f4),
// 13 = Character popup (MENU 0x1f5, filled from the 0x63688a72 family census;
// offscreen in the 0xc1e single-pilot variant). Accept validates both names
// <= 0x18 chars, refusing to close until they pass. Returns true on OK.
bool RunPilotSelectionDialog(SdlPlatform &platform,
                             NovaFontCache &font_cache,
                             GameState &state,
                             const std::function<void()> &render_background) {
  // Variant pick: 0xc1d when two or more non-hidden entries exist, else 0xc1e
  // (stock Nova only ships the hidden .Trader, so its census is 0).
  const auto templates = EnumeratePilotTemplates();
  const int visible = static_cast<int>(std::count_if(
      templates.begin(), templates.end(), [](const PilotTemplateEntry &e) {
        return !e.hidden;
      }));
  const std::uint16_t dialog_id = visible >= 2 ? 0xc1d : 0xc1e;

  auto window = UiWindow_CreateFromDialogResource(platform, dialog_id);
  if (!window) {
    return false;
  }

  // Row 4: the new-pilot (Strict Play) checkbox state from the current flag.
  UiControl_SetValue(*window, 4, state.pilot.strict_play ? 1 : 0);
  // Rows 8/9: random sample Full Name / Nickname from STR# 0x80 rows 1-3 /
  // 4-6 (1-based, like every STR# entry number).
  UiPanel_SetEntryTextPascal(
      *window,
      8,
      NovaHud_LoadStringEntry(
          0x80, static_cast<std::uint16_t>(RandomIndex(state, 3) + 1))
          .value_or(""));
  UiPanel_SetEntryTextPascal(
      *window,
      9,
      NovaHud_LoadStringEntry(
          0x80, static_cast<std::uint16_t>(RandomIndex(state, 3) + 4))
          .value_or(""));
  UiPanel_SetTextEntrySelectionRange(*window, 8, 0, 0xfe);

  // Row 13 (0xc1d only): the Character popup, filled from the census and
  // preselected to the active/default entry. Ghidra preselects via
  // PilotData_FindActivePilotName (0x004cd290, flags bit 0 at block+0x132); a
  // name that is not in the visible census (stock: the hidden .Trader is the
  // active entry) keeps the first row selected.
  if (dialog_id == 0xc1d) {
    std::vector<std::string> names;
    for (const auto &entry : templates) {
      if (!entry.hidden) {
        names.push_back(entry.name);
      }
    }
    const std::string active_pilot = PilotData_FindActivePilotName();
    std::size_t preselect = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (names[i] == active_pilot) {
        preselect = i;
        break;
      }
    }
    UiPanel_SetEntryListItems(*window, 13, "Character", std::move(names));
    UiControl_SetValue(*window, 13, static_cast<short>(preselect + 1));
  }

  short code = -1;
  bool accepted = false;
  bool cancelled = false;
  while (!accepted && !cancelled && !platform.quit_requested()) {
    UiWindow_RunInteractionLoop(
        platform, font_cache, *window, &code, render_background);
    if (code == 1) { // OK: validate both name fields, stay open on violation.
      bool valid = true;
      for (const std::size_t row : {std::size_t{8}, std::size_t{9}}) {
        if (UiPanel_GetEntryTextPascal(*window, row).size() > 0x18) {
          // (The original's FontCache_NoOpCleanup here is a no-op.)
          UiPanel_SetTextEntrySelectionRange(*window, row, 0, 0x17);
          valid = false;
          break;
        }
      }
      accepted = valid;
    }
    if (code == 4) { // Strict Play checkbox: the dialog owns the toggle.
      state.pilot.strict_play = UiControl_GetValue(*window, 4) == 0;
      UiControl_SetValue(*window, 4, state.pilot.strict_play ? 1 : 0);
    }
    if (code == 2) { // Cancel
      cancelled = true;
    }
    code = -1;
  }
  if (!accepted) {
    return false;
  }

  // Copy the names back (the original strips articles/subtitle suffixes from
  // these buffers right after the dialog returns).
  state.pilot.first_name = UiPanel_GetEntryTextPascal(*window, 8);
  state.pilot.last_name = UiPanel_GetEntryTextPascal(*window, 9);
  StripLeadingArticle(state.pilot.first_name);
  StripLeadingArticle(state.pilot.last_name);
  StripSubtitleSuffix(state.pilot.first_name);
  StripSubtitleSuffix(state.pilot.last_name);
  state.pilot.strict_play = UiControl_GetValue(*window, 4) != 0;

  // Row 11: Gender popup selection; the flow latches male on a first char of
  // 'm' (MWRuntime_FUN_004d6230 table lookup compared to 0x6d).
  if (const auto gender = UiPanel_GetEntryTextPascalIndexed(
          *window, 11, UiControl_GetValue(*window, 11))) {
    state.pilot.male =
        !gender->empty() &&
        std::tolower(static_cast<unsigned char>((*gender)[0])) == 'm';
  }
  state.control.male = state.pilot.male;

  // Row 13: the selected character-template name (empty in the 0xc1e variant;
  // Menu_RunNewGameFlow then falls back to the family's first entry).
  if (dialog_id == 0xc1d) {
    if (const auto choice = UiPanel_GetEntryTextPascalIndexed(
            *window, 13, UiControl_GetValue(*window, 13))) {
      state.pilot.character_template = *choice;
    }
  }
  return true;
}

// Ghidra 0x00497900 NovaUi_ShowTextConfirmCodeDialog: the shared text-entry
// modal (DLOG 0xbb9; row 3 = prompt, row 5 = edit text, codes 1 = OK / 6 =
// Cancel). Accept only when the text fits max_chars, else re-select the field
// and continue. Returns the final text on accept, nullopt on cancel.
[[nodiscard]] std::optional<std::string>
NovaUi_ShowTextEntryDialog(SdlPlatform &platform,
                           NovaFontCache &font_cache,
                           std::string_view prompt,
                           std::string_view initial_text,
                           std::int32_t max_chars,
                           const std::function<void()> &render_background) {
  auto window = UiWindow_CreateFromDialogResource(platform, 0xbb9);
  if (!window) {
    return std::nullopt;
  }
  UiPanel_SetEntryTextPascal(*window, 3, prompt);
  UiPanel_SetEntryTextPascal(*window, 5, initial_text);
  UiPanel_SetTextEntrySelectionRange(*window, 5, 0, 0xfe);

  short code = -1;
  bool accepted = false;
  while (!accepted && !platform.quit_requested()) {
    UiWindow_RunInteractionLoop(
        platform, font_cache, *window, &code, render_background);
    if (code == 1) {
      if (static_cast<std::int32_t>(
              UiPanel_GetEntryTextPascal(*window, 5).size()) > max_chars) {
        UiPanel_SetTextEntrySelectionRange(*window, 5, 0, max_chars - 1);
      } else {
        accepted = true;
      }
    }
    if (code == 6) {
      return std::nullopt;
    }
    code = -1;
  }
  return accepted ? std::make_optional(UiPanel_GetEntryTextPascal(*window, 5))
                  : std::nullopt;
}

} // namespace

// ===========================================================================
// Step isolation: data subsystems not yet reconstructed. Each maps to a call
// in Menu_RunNewGameFlow and logs loudly what behaviour it does not reproduce,
// so the new-game path remains reachable end-to-end without silently faking
// the systems it depends on.
namespace {

void Stub_LoadScenarioResourceTables(GameState &state) {
  // Ghidra 0x004bd3c0 NovaData_LoadScenarioResourceTables reads the scenario
  // resource tables (sh\x95p ships, o\x9ftf outfits, w\x91ap weapons,
  // sp\x9ab stellars, s\xd8st systems) into state.scenario, so the player
  // ship reads its real class stats here.
  if (!state.scenario.LoadFromArchives()) {
    NovaLog::Todo("scenario resource tables could not be loaded; player world "
                  "uses fallback defaults");
  }
  // Size the per-system reputation table to the systems table (the original
  // keeps g_system_reputation 0x00733bc8 as a fixed system-indexed int16 span
  // alongside g_system_defs; our default is 0 so every system starts neutral).
  state.system_reputation.assign(state.scenario.systems.size(), 0);
}

void Stub_ResetReputationAndWorldTables(GameState &state) {
  // Ghidra Game_ResetNewGameReputation / Game_ResetNewGameState clear faction
  // standings, mission flags, region networks, and set DAT_00596d35 = 0 so the
  // intro plays on first flight. DAT_00596d35 is our intro_played flag.
  state.intro_played = false;
  NovaLog::Todo("new-game faction reputation and mission flags not tracked; "
                "intro_played latch reset only");
}

void Stub_SeedStartingInventory(GameState &state) {
  // Menu_RunNewGameFlow zeroes the outfit counts and weapon-bank ammo/secondary
  // counters, then seeds them from the starting ship class's default outfit
  // list (DefaultItems) and stock weapon banks. The ship-class tables are
  // available in state.scenario, so for the default ship (id 0x80, zero-based
  // 0) the outfit counts are populated from its default items.
  state.inventory.outfit_owned_count.fill(0);
  state.weapon_bank_ammo.fill(0);
  state.weapon_bank_secondary.fill(0);

  const auto *ship = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (!ship) {
    NovaLog::Todo("starting inventory left empty: default ship class id "
                  "{} not in the scenario tables",
                  state.player.ship_class_id);
    return;
  }
  for (std::size_t i = 0; i < ship->default_outfit_ids.size(); ++i) {
    const auto id = ship->default_outfit_ids[i];
    if (id < 0x80) {
      continue;
    }
    const auto index = static_cast<std::size_t>(id) - 0x80;
    if (index >= state.inventory.outfit_owned_count.size()) {
      continue;
    }
    state.inventory.outfit_owned_count[index] =
        static_cast<std::int16_t>(ship->default_outfit_counts[i]);
  }
  // Seed the weapon banks from the starting ship class's stock weapons,
  // mirroring Menu_RunNewGameFlow: for each stock weapon triple
  // {weapon_id, count, ammo_load} the mounted-count goes into
  // weapon_bank_ammo[weapon_id-0x80] and any carried rounds (ammo_load, when
  // > 0) into the matching secondary/ammo counter. The starter Shuttle's
  // single Light Blaster ({0x80, 1, -1}: 1 mounted, unlimited ammo) thereby
  // lands in bank 0 with weapon_bank_ammo[0] = 1 > 0, so the primary-fire
  // loop (NovaWeapon_FirePlayerPrimary) can fire it. The stock_weapons decode
  // and the loader's default_weapon_ammo/secondary mapping are verified in
  // tests/scenario_data_test.cpp.
  NovaWeapon_SeedBanksFromShipStock(state, state.player.ship_class_id);
  // Reset any lingering per-bank cooldown so a fresh pilot can fire
  // immediately on entering spaceflight.
  state.weapon_bank_cooldown.fill(0.0F);
  state.active_shots.clear();
  // Menu_RunNewGameFlow calls Weapon_ReconcileOutfitPoolWithWeaponBanks right
  // after seeding the weapon banks: leftover bank ammo/secondary not explained
  // by an owned outfit is converted back into owned weapon/ammo outfits. This
  // registers the Shuttle's stock Light Blaster (a mounted bank with no
  // DefaultItem entry) as an owned outfit, so the Outfitter lists it as owned,
  // it can be sold, and it survives the later bank rebuilds (buy/sell/close).
  NovaWeapon_ReconcileOutfitPoolWithWeaponBanks(state);
  // ResetPlayerShipForNewGame calculated capacities before this inventory was
  // seeded. Recompute so the new pilot starts with installed bonuses.
  OutfitMarkStatsDirty(state);
  const PlayerEffectiveStats effective =
      Outfit_ComputePlayerEffectiveStats(state);
  state.player.shield_points = effective.max_shield_points;
  state.player.armor_points = effective.max_armor_points;
  state.player.fuel_points = effective.fuel_capacity;
  state.cached_stats = effective;
  state.stat_cache_valid = true;
  NovaLog::Info("new-game inventory seeded from ship class '{}' ({} default "
                "outfits)",
                ship->display_name,
                std::count_if(ship->default_outfit_ids.begin(),
                              ship->default_outfit_ids.end(),
                              [](std::int16_t id) { return id >= 0x80; }));
}

void Stub_DiscoverStartingSystems(GameState &state) {
  // The original books the starting system as visited (discovery_state = 1,
  // both the ship-init and per-tick latches in Ship_HandlePlayerShipCore
  // 0x0044aa70) and rebuilds the map reveal from it
  // (System_RebuildSystemVisibilityMap(cur, 0, 1)). Only the start system is
  // marked visited; its linked neighbours show on the starmap via the
  // discovered_this_rebuild latch without becoming explored themselves.
  NovaSystem_OnSystemEntered(state, kStartSystemId, 1);
  NovaLog::Info("starting system discovery seeded: {} (resource {}) and "
                "adjacent neighbours now visible on the starmap",
                kStartSystemId,
                kStartSystemResourceId);
}

void Stub_PickFirstTravelDestination(GameState &state) {
  // Ghidra Stellar_FindNearestAvailableTravelStellar picks the first adjacent,
  // reachable travel point as the pilot's initial jump target. The travel
  // mechanics are reconstructed (travel.cpp): the first-jump target is
  // resolved dynamically at engage time, so this step just validates that the
  // starting system has a reachable outward route and logs it.
  const int slot = NovaTravel_FindNearestTravelPoint(state);
  if (slot >= 0) {
    NovaLog::Info("initial travel outline resolved from starting-system "
                  "nav-defs: travel slot {} available",
                  slot);
  } else {
    NovaLog::Todo("first travel destination not resolved: starting system has "
                  "no reachable travel point defined");
  }
}

void RecomputePlayerMeters(GameState &state) {
  // Shield/armor/fuel are recomputed from the default ship class plus any
  // owned outfit bonuses (the effective-stats aggregation at Ghidra
  // 0x00463550/0x004637a0/0x00463a20), so a ship stocked with shield/armor/
  // fuel outfits starts full. Menu_RunNewGameFlow recomputes these AFTER the
  // starting outfit counts are seeded, so this helper runs after the inventory
  // seed rather than during the ship-state reset. Falls back to the class
  // template defaults when the class table is unavailable.
  state.stat_cache_valid = false;
  const game::PlayerEffectiveStats eff =
      game::Outfit_ComputePlayerEffectiveStats(state);
  state.player.shield_points = eff.max_shield_points;
  state.player.armor_points = eff.max_armor_points;
  state.player.fuel_points = eff.fuel_capacity;
  state.cached_stats = eff;
  state.stat_cache_valid = true;
  NovaLog::Debug("player ship reset for new game: shield {}, armor {}, fuel {}",
                 eff.max_shield_points,
                 eff.max_armor_points,
                 eff.fuel_capacity);
}

void ResetPlayerShipForNewGame(GameState &state) {
  // Ghidra 0x004b3350 Ship_ResetPlayerShipState: fresh position/velocity,
  // default class id, cleared targeting/travel/mission/AI fields and debuffs.
  // The param_1!=0 fresh-game branch also seeds the player's starting credits
  // to 10000 (g_ship_states->credits = 10000). Shield/armor/fuel are NOT
  // finalized here: Menu_RunNewGameFlow recomputes them after the starting
  // outfit counts are seeded (see RecomputePlayerMeters).
  //
  // Start in the hardcoded starting system (Tichel for now; the original
  // randomizes among a few start candidates). The world coordinate axes follow
  // the game convention confirmed from the flight renderer: heading 0 = up
  // (-y), so world +y is down on screen (Math_AddPolarVelocity projects
  // heading -> vel = (sin, -cos); pos += vel * dt).
  state.player.current_system_id = kStartSystemId;
  state.player.credits = 10000;

  state.player.pos_x = 0.0F;
  // Spawn just below the starting system's landing stellar so the body is
  // visible ahead rather than under the ship. The original positions the ship
  // adjacent to the planet it just left; the offset is provisional until the
  // landing/launch placement is reconstructed.
  state.player.pos_y = 60.0F;
  if (const auto *sys = state.scenario.System(kStartSystemResourceId); sys) {
    if (const auto anchor_id = PickLandingStellarResource(sys->nav_defs);
        anchor_id >= 0x80) {
      if (const auto *anchor = state.scenario.Stellar(anchor_id); anchor) {
        // Spawn just east (positive-x screen = starboard) of the anchor body,
        // a small pull away so the planet stays framed ahead-down.
        state.player.pos_x = static_cast<float>(anchor->pos_x) + 30.0F;
        state.player.pos_y = static_cast<float>(anchor->pos_y) + 60.0F;
        NovaLog::Debug("spawning new pilot near landing stellar '{}' at "
                       "({}, {})",
                       anchor->name,
                       anchor->pos_x,
                       anchor->pos_y);
      }
    }
  } else {
    NovaLog::Todo("starting system {:#x} not in scenario tables; spawn kept at "
                  "the origin",
                  kStartSystemResourceId);
  }

  state.player.vel_x = state.player.vel_y = 0.0F;
  state.player.heading = 0.0F;
  state.player.speed = 0.0F;
  state.player.ship_class_id = state.pilot.start_type_code; // ch r ShipType
  // Ship_ResetPlayerShipState leaves the active weapon bank unselected (-1);
  // the firing loop (NovaWeapon_FirePlayerPrimary) fires every loaded bank
  // regardless, so the selection latch is only carried for save/UI fidelity.
  state.player.active_weapon_bank_slot = -1;
  state.player.timed_action_counter = -1;
  state.player.death_timer_active = -1.0F;
  state.player.is_active = true;
  // Clear the death / escape-pod latches a previous flight may have left set
  // (the original's new-game reset path clears DAT_00596d38 / DAT_007354a5).
  state.game_over_pending = false;
  state.return_to_menu_pending = false;
  state.distress_cue_active = false;
  state.distress_cue_active_prev = false;
  state.bomb_detonation_timer = 0.0F;
  state.recently_hit_timer = 0.0F;
}

void SetNewGameDateAndStrings(GameState &state) {
  // The original copies the starting year/month/day and sets the opener string
  // globals (DAT_005997cc / DAT_005999cc). The in-engine date drives the
  // pause-menu clock and event scheduling; it is not surfaced in this build.
  // The opener strings are stored on state->pilot by the naming step above.
  (void)state;
  NovaLog::Debug("new-game date and opener strings set (in build: opener "
                 "strings stored on pilot only)");
}

} // namespace

bool NovaNewPilotFlow_Run(SdlPlatform &platform,
                          GameState &state,
                          const std::function<void()> &render_background) {
  // ---- Step 1: pilot naming/selection ------------------------------------
  // Ghidra: Menu_RunNewGameFlow prefills DAT_007d20b7/DAT_007d21b7 with random
  // STR# 0x80 sample names (done inside the dialog port) and runs
  // Menu_RunPilotSelectionDialog (0x0048a7e0), which fills the Full Name /
  // Nickname fields, the Strict Play flag, the Gender popup and the character
  // template; the port strips articles/subtitle suffixes inside the dialog
  // port, matching the original's post-accept strip.
  NovaFontCache font_cache;
  if (!RunPilotSelectionDialog(
          platform, font_cache, state, render_background)) {
    return false;
  }

  // The 0xc1e variant leaves the Character popup offscreen; the flow falls
  // back to the family's first entry (stock: the hidden .Trader).
  if (state.pilot.character_template.empty()) {
    const auto templates = EnumeratePilotTemplates();
    if (!templates.empty()) {
      state.pilot.character_template = templates.front().name;
    }
  }
  state.pilot.start_type_code =
      ResolveStartTypeFromTemplate(state.pilot.character_template);

  // ---- Step 2: ship christening -------------------------------------------
  // Ghidra: NovaUi_ShowTextConfirmCodeDialog (DLOG 0xbb9) with prompt =
  // STR# 0x7d2 row 0x79 + the start class's long name (DAT_005a9bcc table;
  // the port reads Ship::long_name from the scenario tables) and initial text
  // = a random STR# 0x80 row 7-9 ship name, max 0x40 chars. The result is
  // article-stripped into the ship-name global (DAT_00599acc).
  {
    const std::string class_caption =
        state.scenario.Ship(
            static_cast<std::int16_t>(state.pilot.start_type_code + 0x80))
            ? state.scenario
                  .Ship(static_cast<std::int16_t>(state.pilot.start_type_code +
                                                  0x80))
                  ->long_name
            : "";
    std::string prompt;
    // STR# 0x7d2 row 0x79 (1-based): "Now, please christen your brand-new".
    if (auto prefix = NovaHud_LoadStringEntry(0x7d2, 0x79)) {
      prompt = *prefix + " ";
    } else {
      NovaLog::Todo("STR# 0x7d2 row 0x79 (christening prompt prefix) "
                    "unavailable");
    }
    prompt += class_caption + ": ";
    // STR# 0x80 rows 7-9 (1-based): the random suggested ship names ('Ring
    // of Glory', 'Snowy Owl', 'Cardinal Virtue').
    const std::string suggested =
        NovaHud_LoadStringEntry(
            0x80, static_cast<std::uint16_t>(RandomIndex(state, 3) + 7))
            .value_or("");
    auto ship_name = NovaUi_ShowTextEntryDialog(
        platform, font_cache, prompt, suggested, 0x40, render_background);
    if (!ship_name) {
      return false;
    }
    StripLeadingArticle(*ship_name);
    state.player.ship_name = std::move(*ship_name);
  }

  // ---- Step 3: overwrite-existing-pilot confirmation ----------------------
  // Ghidra: builds <nova_files><name>.plt, PilotFile_ProbeExists, and on hit
  // asks "overwrite?" via Ui_ShowConfirmDialog. The reimplementation does not
  // create pilot save files (.plt) yet, so there is never an existing pilot to
  // confirm against; the overwrite prompt is skipped entirely.
  NovaLog::Todo("pilot save files (.plt) are not written; overwrite check "
                "skipped so a like-named pilot is never rejected");

  // ---- Step 4: fresh-world reset ------------------------------------------
  // Ghidra: g_travel_interaction_loop_active = 0, Ship_ResetPlayerShipState,
  // Game_ResetNewGameReputation/State, zero outfit/weapon tables,
  // PilotData_InitializePlayerState, clear system discovery, re-seed the
  // starting inventory, discover surrounding systems, load scenario tables.
  // Ghidra reseeds the global LCG once at session bootstrap
  // (NovaRandom_Reseed 0x004ab970 from NovaGameSession_Run) so each game's
  // NovaRandom draws differ; the clean-room GameState keeps its own mt19937
  // (default-seeded 42), so reseed it with fresh entropy here or every new
  // pilot would spawn the same deterministic ships/positions. This runs before
  // the scenario load + opener-string rolls below (which draw from the rng).
  NovaGame_ReseedRandom(state);
  // Ghidra loads the scenario data tables (ships/outfits/weapons/stellars/
  // systems) as part of the fresh world reset; the ship reset and inventory
  // seed below read class stats, so load the tables first.
  Stub_LoadScenarioResourceTables(state);
  ResetPlayerShipForNewGame(state);
  Stub_ResetReputationAndWorldTables(state);
  Stub_SeedStartingInventory(state);
  // Menu_RunNewGameFlow finalizes shield/armor/fuel only after the starting
  // outfit counts and weapon banks are seeded, so their max capacities account
  // for the starter loadout's outfit bonuses.
  RecomputePlayerMeters(state);
  Stub_DiscoverStartingSystems(state);
  SetNewGameDateAndStrings(state);

  // ---- Step 5: first travel destination + scenario spawn ------------------
  Stub_PickFirstTravelDestination(state);
  // Ghidra: System_RebuildInitialNpcAndMissionPopulation +
  // System_UpdateSystemAndStellar display state + Asteroid_InitSystem (the
  // system's asteroid field). The mission-fleet restore slice and the initial
  // System.avg_ships ambient slice are both reconstructed.
  NovaSystem_RestoreMissionFleets(state,
                                  state.player.current_system_id,
                                  /*copy_player_heading=*/false,
                                  SDL_GetTicks());
  // First offering roll for the fresh world (Ship_InitGameplayDataTables
  // 0x00458802 arm also re-rolls on arrival; the loader zeroed the table).
  Mission_RerollOfferingRolls(state);
  NovaSystem_PopulateInitialNpcShips(state, state.player.current_system_id);
  // TODO(decomp(0x00489d70)) skipped scopes from Menu_RunNewGameFlow's
  // fresh-world tail, each with a known original call:
  //   - Ship_DeactivateVacantShipsAndTally(1)
  //   - Frame_TriggerSystemRegionEvents(current system)
  //   - NovaResources_EvaluateAvailability() (per-stellar availability rolls)
  //   - the stellar hazard-marker pass (availability flags 0x20/0x40 over all
  //     0x800 stellars)
  //   - the live date-block copy (g_current_game_year_month/day, DAT_00735460)
  //   - starmap pan origin init + current-system field_0xc8/0xc4
  //   - per-ship zeroing of ionization_points/field_0xb0/
  //     turn_bank_animation_phase/ai_turn_bias_dir and DAT_007cab1c = 0xfffd
  //   - the second PilotData_InitializePlayerState pass (param 0) after the
  //     availability rolls
  //   - PilotFile_SaveGame(final stellar) — no .plt writer yet (Step 3 log)

  // ---- Step 6: assemble the persistent pilot record and apply it ----------
  // Ghidra keeps the freshly-seeded pilot in a pilot-save block (resource id
  // 0x63688a72) created/accessed by PilotData_InitializePlayerState and
  // IntroCinematic_SetupFrames; the running game only materializes that
  // record on demand (see pilot_file.hpp). The reimplementation does the
  // same: build a PilotFile for the new pilot, then copy it into the live
  // GameState so the intro and spaceflight read one consistent record.
  PilotFile record = PilotFile::Fresh();
  record.pilot_name = state.pilot.first_name;
  // A fresh pilot block is not a loaded save: PilotData_InitializePlayerState
  // leaves Ship_ResetPlayerShipState's live values in place, then the original
  // refills shield, armor and fuel after stock outfits are seeded. Our
  // in-memory record is applied below solely to keep the tracked intro/save
  // fields together, so it must carry those live values rather than its zero
  // initialization. Otherwise PilotFileApply would incorrectly erase the
  // newly filled ship and the seeded inventory.
  record.credits = state.player.credits;
  record.ship_class_id = state.player.ship_class_id;
  record.current_system_id = state.player.current_system_id;
  record.active_weapon_bank_slot = state.player.active_weapon_bank_slot;
  record.timed_action_counter = state.player.timed_action_counter;
  record.death_timer_active = state.player.death_timer_active;
  record.shield_points = state.player.shield_points;
  record.armor_points = state.player.armor_points;
  record.fuel_points = state.player.fuel_points;
  record.pos_x = state.player.pos_x;
  record.pos_y = state.player.pos_y;
  record.vel_x = state.player.vel_x;
  record.vel_y = state.player.vel_y;
  record.heading = state.player.heading;
  record.speed = state.player.speed;

  // Carry the weapon banks seeded in Step 4 (from the starting ship's stock
  // weapons) into the record, otherwise PilotFileApply below copies a fresh
  // record whose banks are all zero and clobbers the seeded Light Blaster
  // bank 0, so nothing could ever fire.
  record.weapon_bank_ammo = state.weapon_bank_ammo;
  record.weapon_bank_secondary = state.weapon_bank_secondary;
  record.outfit_owned_count = state.inventory.outfit_owned_count;

  // Copy the assembled record into the live state (mirroring the block-to-
  // global copy IntroCinematic_SetupFrames/PilotData_InitializePlayerState
  // perform). The intro and spaceflight modes read these live fields. The
  // record is kept in memory only (no .plt writer); see PilotFileApply.
  PilotFileApply(record, state);
  // Ghidra 0x004cd3b0 IntroCinematic_SetupFrames: reads the keyed pilot block
  // (selected character template; absent block -> the no-save default) and
  // fills g_intro_cinematic, clamping ids/durations. Stock data: block
  // ".Trader" -> PICTs 0x2008/0x2009/0x200a for 45 ticks each, post-intro
  // destination -1 (no dialog).
  NovaIntroCinematic_SetupFrames(state, state.pilot.character_template);
  NovaLog::Info("new-game intro configured: frames {} {} {} {} for {} {} {} {} "
                "ticks each (pilot block '{}', post-intro destination {})",
                state.intro_cinematic.source_pict_ids[0],
                state.intro_cinematic.source_pict_ids[1],
                state.intro_cinematic.source_pict_ids[2],
                state.intro_cinematic.source_pict_ids[3],
                state.intro_cinematic.duration_60h_ticks[0],
                state.intro_cinematic.duration_60h_ticks[1],
                state.intro_cinematic.duration_60h_ticks[2],
                state.intro_cinematic.duration_60h_ticks[3],
                state.pilot.character_template,
                state.intro_cinematic.post_intro_dest_id);

  // ---- Step 7: mark active ------------------------------------------------
  // Ghidra: DAT_00596d28 = 1 (game active), DAT_00596d2f = the dialog's
  // Strict Play checkbox state (latched by the dialog port into
  // state.pilot.strict_play).
  state.game_active = true;
  NovaLog::Info("new pilot active: callsign '{}', start type {}",
                state.pilot.first_name,
                state.pilot.start_type_code);
  return true;
}

} // namespace game
