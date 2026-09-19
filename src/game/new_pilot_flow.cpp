#include "new_pilot_flow.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "government.hpp"
#include "hud_overlay.hpp"
#include "intro_cinematic.hpp"
#include "mission.hpp"
#include "mission_script.hpp"
#include "nova_font.hpp"
#include "nova_name_text.hpp"
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
#include <ctime>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace game {
namespace {

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

// Selections captured by the new-pilot dialogs. Held in locals until every
// cancellable prompt (pilot selection, overwrite, christening) has succeeded,
// mirroring the original's DAT_007d20b7 / DAT_007d21b7 / DAT_007d22b7 /
// DAT_007d23b7 + *is_new_pilot_flag / DAT_007d4c0e temporaries; the persistent
// g_player_* globals are only written in Menu_RunNewGameFlow's success tail
// (0x0048a51e onward), so a cancel never touches the running pilot.
struct NewPilotDraft {
  std::string first_name;
  std::string last_name;
  std::string character_template;
  std::string ship_name;
  // Menu_RunNewGameFlow initializes *is_new_pilot_flag to 0 before calling the
  // selection dialog, so a fresh draft starts unchecked regardless of the
  // active pilot's g_strict_play.
  bool strict_play = false;
  bool male = false;
  std::int16_t start_type_code = 0;
};

// Ghidra 0x0048a7e0 Menu_RunPilotSelectionDialog. DITL rows (1-based):
// 4 = Strict Play checkbox (code 4), 8/9 = Full Name / Nickname edit texts
// (prefilled from STR# 0x80 rows 1-3 / 4-6), 11 = Gender popup (MENU 0x1f4),
// 13 = Character popup (MENU 0x1f5, filled from the 0x63688a72 family census;
// offscreen in the 0xc1e single-pilot variant). Accept validates both names
// <= 0x18 chars, refusing to close until they pass. Writes only `draft` on
// accept; it consumes `state.rng` but does not write any live pilot state.
// Returns true on OK.
bool RunPilotSelectionDialog(SdlPlatform &platform,
                             NovaFontCache &font_cache,
                             GameState &state,
                             NewPilotDraft &draft,
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

  // Row 4: the new-pilot (Strict Play) checkbox state, from the draft (the
  // original initializes its flag to 0 before the call).
  UiControl_SetValue(*window, 4, draft.strict_play ? 1 : 0);
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

  ProbeUiAutoClear probe_ui_guard(platform);
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
      draft.strict_play = UiControl_GetValue(*window, 4) == 0;
      UiControl_SetValue(*window, 4, draft.strict_play ? 1 : 0);
    }
    if (code == 2) { // Cancel
      cancelled = true;
    }
    code = -1;
  }
  if (!accepted || platform.quit_requested()) {
    return false;
  }

  // Copy the names into the draft (the original strips articles/subtitle
  // suffixes from these buffers right after the dialog returns).
  draft.first_name = UiPanel_GetEntryTextPascal(*window, 8);
  draft.last_name = UiPanel_GetEntryTextPascal(*window, 9);
  StripLeadingArticle(draft.first_name);
  StripLeadingArticle(draft.last_name);
  draft.first_name = NovaText_StripSubtitleSuffix(draft.first_name);
  draft.last_name = NovaText_StripSubtitleSuffix(draft.last_name);
  draft.strict_play = UiControl_GetValue(*window, 4) != 0;

  // Row 11: Gender popup selection; the flow latches male on a first char of
  // 'm' (MWRuntime_FUN_004d6230 table lookup compared to 0x6d).
  if (const auto gender = UiPanel_GetEntryTextPascalIndexed(
          *window, 11, UiControl_GetValue(*window, 11))) {
    draft.male = !gender->empty() &&
                 std::tolower(static_cast<unsigned char>((*gender)[0])) == 'm';
  }

  // Row 13: the selected character-template name (empty in the 0xc1e variant;
  // Menu_RunNewGameFlow then falls back to the family's first entry).
  if (dialog_id == 0xc1d) {
    if (const auto choice = UiPanel_GetEntryTextPascalIndexed(
            *window, 13, UiControl_GetValue(*window, 13))) {
      draft.character_template = *choice;
    }
  }
  return true;
}

} // namespace

// ===========================================================================
// Step isolation: data subsystems not yet reconstructed. Each maps to a call
// in Menu_RunNewGameFlow and logs loudly what behaviour it does not reproduce,
// so the new-game path remains reachable end-to-end without silently faking
// the systems it depends on.
namespace {

void Stub_LoadScenarioResourceTables(GameState &state, bool ship_animations) {
  // Ghidra 0x004bd3c0 NovaData_LoadScenarioResourceTables reads the scenario
  // resource tables (sh\x95p ships, o\x9ftf outfits, w\x91ap weapons,
  // sp\x9ab stellars, s\xd8st systems) into state.scenario, so the player
  // ship reads its real class stats here. Menu_RunNewGameFlow calls this on
  // EVERY new game (0x0048a4f8), so the reload also resets every resource-
  // derived mutable field (system discovery_state, personality alive/grudge,
  // nebula explored latches, stellar destroyed/present counts); without it a
  // second new game in one session inherits the previous pilot's galaxy.
  if (!state.scenario.LoadFromArchives(&state.rng, ship_animations)) {
    NovaLog::Todo("scenario resource tables could not be loaded; player world "
                  "uses fallback defaults");
  }
  // Ghidra 0x0043bbb0 NovaResources_LoadMisnResourceDefs (runtime half):
  // clear the mission interaction latches so a second new game cannot inherit
  // the previous pilot's state.
  Mission_ResetRuntimeStateOnMissionDefsLoad(state);
  // Size the per-system reputation table to the systems table (the original
  // keeps g_system_reputation 0x00733bc8 as a fixed system-indexed int16 span
  // alongside g_system_defs; our default is 0 so every system starts neutral).
  state.system_reputation.assign(state.scenario.systems.size(), 0);
}

void Stub_ResetReputationAndWorldTables(GameState &state) {
  // Ghidra Game_ResetReputationAndAvailability (0x004b4220) and
  // Game_ResetNewGameState (0x004b4690). The latter zeroes the whole
  // g_nova_control_bits array (0x004b477e loop): a second new game must not
  // inherit the previous pilot's story/mission control bits. Gender and the
  // license `registered` flag are separate globals and stay untouched, so
  // preserve them here. Re-seed the clean-room ferry baseline (b311).
  state.control.bits.reset();
  state.control.persisted_bit_bytes.fill(0);
  state.control.SetControlBit(311, true);
  state.control.map_grant_latch = false;
  state.control.record_grant_latch = false;
  // Game_ResetReputationAndAvailability 0x004b4220 param_1 != 0 zeroes the
  // player combat rating and clears the recently-activated rank latch. The
  // character template (ApplyCharacterTemplate) overwrites the rating when
  // present; without this a template-less pilot would inherit the previous
  // pilot's rating.
  state.player_combat_rating_points = 0;
  state.recently_activated_rank_id = -1;
  state.intro_played = false;
  NovaLog::Todo("new-game faction reputation and mission flags not tracked; "
                "control bits and intro_played latch reset only");
}

void Stub_SeedStartingInventory(GameState &state) {
  // Menu_RunNewGameFlow zeroes the outfit counts and weapon-bank ammo/secondary
  // counters, then seeds them from the starting ship class's default outfit
  // list (DefaultItems) and stock weapon banks. The ship-class tables are
  // available in state.scenario, so for the default ship (id 0x80, zero-based
  // 0) the outfit counts are populated from its default items.
  state.inventory.outfit_owned_count.fill(0);
  state.weapon_count_by_class.fill(0);
  state.weapon_secondary_count_by_class.fill(0);

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
  // weapon_count_by_class[weapon_id-0x80] and any carried rounds (ammo_load,
  // when > 0) into the matching secondary/ammo counter. The starter Shuttle's
  // single Light Blaster ({0x80, 1, -1}: 1 mounted, unlimited ammo) thereby
  // lands in bank 0 with weapon_count_by_class[0] = 1 > 0, so the primary-fire
  // loop (NovaWeapon_TickPlayerWeaponCommands primary-fire arm) can fire it.
  // The stock_weapons decode and the loader's default_weapon_ammo/secondary
  // mapping are verified in tests/scenario_data_test.cpp.
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
  NovaOutfit_RecomputeOutfitDerivedState(state);
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
  // Ghidra Menu_RunNewGameFlow 0x0048a0f0 explicitly zeroes every system's
  // discovery_state before the mark pass, then marks the current system's
  // discovery slot (0x0048a1a0) and every direct adjacency whose target is
  // visible (0x0048a17e) as visited (discovery_state = 1). The unconditional
  // scenario reload in Stub_LoadScenarioResourceTables already clears the fog,
  // but keep the zero pass here so the reset is visible and survives any
  // future loader change.
  for (System &system : state.scenario.systems) {
    system.discovery_state = 0;
    system.discovered_this_rebuild = false;
  }
  const std::int16_t start_system_id = state.player.current_system_id;
  const std::int16_t start_system_resource_id =
      static_cast<std::int16_t>(start_system_id + 0x80);
  if (start_system_id < 0 || static_cast<std::size_t>(start_system_id) >=
                                 state.scenario.systems.size()) {
    NovaLog::Todo("starting system {} out of range; discovery left empty",
                  start_system_id);
    return;
  }

  // Current system and its discovery slot (the visibility-twin root).
  NovaSystem_MarkSystemVisited(state, start_system_id, 1);
  NovaSystem_MarkSystemVisited(
      state, NovaSystem_ResolveDiscoverySlot(state, start_system_id), 1);

  // Direct adjacency: the original walks the system's 16 Con slots and books
  // each visible target's discovery slot as visited. The port's links are
  // already normalized to visibility roots and stored as 0x80-based resource
  // ids (scenario_data.cpp link pass), so mark each link's discovery slot.
  for (const std::int16_t link :
       state.scenario.systems[static_cast<std::size_t>(start_system_id)]
           .links) {
    if (link < 0x80) {
      continue;
    }
    const auto target = static_cast<std::int16_t>(link - 0x80);
    if (!NovaSystem_IsSystemVisible(state, target)) {
      continue;
    }
    NovaSystem_MarkSystemVisited(
        state, NovaSystem_ResolveDiscoverySlot(state, target), 1);
  }

  // Ghidra NovaResources_EvaluateAvailability 0x00448090 runs at game-session
  // bootstrap: filter every system's is_visible through its Visibility NCB
  // (hiding the invisible story-twin clones) before anything reads it. The
  // reveal latch is rebuilt afterwards, mirroring
  // System_UpdateSystemAndStellarDisplayState later in the original flow.
  NovaResources_EvaluateAvailability(state);
  NovaSystem_RebuildDiscoveredLatch(state);
  NovaLog::Info("starting system discovery seeded: {} (resource {}) and "
                "adjacent neighbours now visited on the starmap",
                start_system_id,
                start_system_resource_id);
}

[[nodiscard]] std::int16_t PickFirstSaveStellar(const GameState &state) {
  // Ghidra Stellar_FindNearestAvailableTravelStellar picks the first adjacent,
  // reachable travel point as the pilot's initial jump target. The travel
  // mechanics are reconstructed (travel.cpp): the first-jump target is
  // resolved dynamically at engage time, so this step just validates that the
  // starting system has a reachable outward route and logs it. Returns the
  // 0-based g_stellar_defs index the original saves at block1+0x00 (the port's
  // nav_defs are 0x80-based resource ids, so rebase).
  const int slot = NovaTravel_FindNearestTravelPoint(state);
  if (slot >= 0) {
    const System *system = state.scenario.System(
        static_cast<std::int16_t>(state.player.current_system_id + 0x80));
    if (system != nullptr) {
      return PilotFileStellarIndexFromResourceId(
          system->nav_defs[static_cast<std::size_t>(slot)]);
    }
  }
  // 0x00489d70 falls back to the first nonnegative nav stellar, then passes
  // zero (g_stellar_defs index 0) when the system has no nav stellar at all.
  const System *system = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (system != nullptr) {
    const auto fallback = std::ranges::find_if(
        system->nav_defs, [](std::int16_t stellar) { return stellar >= 0x80; });
    if (fallback != system->nav_defs.end()) {
      return PilotFileStellarIndexFromResourceId(*fallback);
    }
  }
  return 0;
}

void RecomputePlayerMeters(GameState &state) {
  // Shield/armor/fuel are recomputed from the default ship class plus any
  // owned outfit bonuses (the effective-stats aggregation at Ghidra
  // 0x00463550/0x004637a0/0x00463a20), so a ship stocked with shield/armor/
  // fuel outfits starts full. Menu_RunNewGameFlow recomputes these AFTER the
  // starting outfit counts are seeded, so this helper runs after the inventory
  // seed rather than during the ship-state reset. Falls back to the class
  // template defaults when the class table is unavailable.
  state.InvalidateDerivedStatCaches();
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
  // The system-dependent placement (start system, reinforcement timer, starmap
  // pan, spawn position) is applied later by PlacePlayerInStartSystem, after
  // PilotData_InitializePlayerState has picked the character template's start
  // system. Ship_ResetPlayerShipState leaves current_system_id = 0.
  // The world coordinate axes follow the game convention confirmed from the
  // flight renderer: heading 0 = up (-y), so world +y is down on screen
  // (Math_AddPolarVelocity projects heading -> vel = (sin, -cos); pos += vel *
  // dt).
  NovaShip_ResetPlayerShipState(state);
  state.player.credits = 10000;
  state.player.ship_class_id = state.pilot.start_type_code; // ch r ShipType
  // Ship_ResetPlayerShipState leaves the active weapon bank unselected (-1);
  // the firing loop (NovaWeapon_TickPlayerWeaponCommands) fires every loaded
  // bank regardless, so the selection latch is only carried for save/UI
  // fidelity.
}

// Applies the system-dependent placement Menu_RunNewGameFlow performs after
// PilotData_InitializePlayerState: starting-system reinforcement countdown
// (0x0048a1b4), starmap pan origin (0x0048a3a5) and the provisional spawn
// beside the landing stellar.
void PlacePlayerInStartSystem(GameState &state) {
  const std::int16_t system_id = state.player.current_system_id;
  const std::int16_t system_resource_id =
      static_cast<std::int16_t>(system_id + 0x80);
  // Ghidra Menu_RunNewGameFlow 0x0048a1b4: the starting system's reinforcement
  // countdown is armed to -1 with a zero cooldown so a fresh pilot does not
  // inherit the previous pilot's pending enemy spawn timer.
  if (system_id >= 0 && static_cast<std::size_t>(system_id) <
                            state.reinforcement_countdown.size()) {
    state.reinforcement_countdown[static_cast<std::size_t>(system_id)] = -1.0F;
    state.reinforcement_retrigger_delay[static_cast<std::size_t>(system_id)] =
        0;
  }
  // Ghidra 0x0048a3a5 (Menu_RunNewGameFlow): the starmap pan origin starts on
  // the starting system's position so the map opens centred there.
  if (const auto *start_sys = state.scenario.System(system_resource_id)) {
    state.starmap_pan_x = static_cast<float>(start_sys->pos_x);
    state.starmap_pan_y = static_cast<float>(start_sys->pos_y);
  }

  state.player.pos_x = 0.0F;
  // Spawn just below the starting system's landing stellar so the body is
  // visible ahead rather than under the ship. The original positions the ship
  // adjacent to the planet it just left; the offset is provisional until the
  // landing/launch placement is reconstructed.
  state.player.pos_y = 60.0F;
  if (const auto *sys = state.scenario.System(system_resource_id); sys) {
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
                  system_resource_id);
  }
}

void SetNewGameDateAndStrings(GameState &state) {
  // Ghidra 0x004b4690 Game_ResetNewGameState -> DrawContext_ReadRenderParams
  // (0x004bbb10) + FUN_004fdcb0: the in-game calendar is seeded from the
  // local clock — the real month/day/year with the year advanced by 250
  // (0xfa). The opener strings are stored on state->pilot by the naming step
  // above.
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  // localtime_r is POSIX; keep the fallback for non-POSIX hosts.
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  state.date.year = static_cast<std::int16_t>(local.tm_year + 1900 + 250);
  state.date.month = static_cast<std::int16_t>(local.tm_mon + 1);
  state.date.day = static_cast<std::int16_t>(local.tm_mday);
}

// Ghidra 0x004cd4b0 PilotData_InitializePlayerState (param_2 != 0): overlays
// the selected character template's starting state onto the freshly reset
// world. Credits, combat rating, per-system reputation and the calendar replace
// the Ship_ResetPlayerShipState / Game_ResetNewGameState seeds. Returns the
// OnStart control-bit set string for the caller to run once the world reset is
// done (the original's param_2 == 0 pass, just before the initial save).
std::string ApplyCharacterTemplate(GameState &state) {
  const auto tmpl = CharacterTemplate_Read(state.pilot.character_template);
  if (!tmpl) {
    // Absent-block seed (0x004cd4b0): 10000 credits/class 0/system 0 already
    // applied elsewhere, calendar 2250/1/1, no date prefix/suffix.
    state.player.current_system_id = 0;
    state.date = GameDate{2250, 1, 1};
    state.date_prefix.clear();
    state.date_suffix.clear();
    NovaLog::Todo("new pilot: character template '{}' not found; keeping the "
                  "absent-block defaults",
                  state.pilot.character_template);
    return {};
  }

  state.player.credits = std::max<std::int32_t>(tmpl->credits, 0);
  state.player_combat_rating_points = tmpl->combat_rating_points;
  // Start system: the original's rejection pick among System1-4 (+0x06).
  // Ship_ResetPlayerShipState already wrote 0, and PilotData_PickStartingSystem
  // returns 0 without consuming RNG when no slot is valid, so the absent/
  // template-less path stays on system 0 (no Tichel hardcode).
  state.player.current_system_id =
      PilotData_PickStartingSystem(*tmpl, state.rng);
  // Per-govt starting legal record: for each present Govt entry, apply Status
  // to every system owned by an allied government and negate it for that
  // government's enemies (the original's 4 x 0x800 double loop, which leaves
  // neutral systems untouched). Stock .Trader has all Govt/Status = -1, so
  // nothing changes; mod templates can start the player criminal or legal.
  for (std::size_t i = 0; i < tmpl->govt_ids.size(); ++i) {
    if (tmpl->govt_ids[i] < 0x80) {
      continue;
    }
    const auto govt = static_cast<std::int16_t>(tmpl->govt_ids[i] - 0x80);
    const auto status = tmpl->govt_status[i];
    for (std::size_t s = 0; s < state.system_reputation.size(); ++s) {
      const auto system_govt = state.scenario.systems[s].government_id;
      if (NovaGovernment_AreGovtsAllied(state.scenario, system_govt, govt)) {
        state.system_reputation[s] = status;
      } else if (NovaGovernment_AreGovtsHostileOrXenophobic(
                     state.scenario, system_govt, govt)) {
        state.system_reputation[s] = static_cast<std::int16_t>(-status);
      }
    }
  }

  state.date = tmpl->date;
  state.date_prefix = tmpl->date_prefix;
  state.date_suffix = tmpl->date_suffix;
  NovaLog::Info("character template '{}': {} credits, combat rating {}, start "
                "date {}-{:02}-{:02}, date suffix '{}'",
                state.pilot.character_template,
                state.player.credits,
                state.player_combat_rating_points,
                state.date.year,
                state.date.month,
                state.date.day,
                state.date_suffix);
  return tmpl->on_start;
}

// Ghidra Game_ResetNewGameState (0x004b4690), 0x004b46bc..0x004b4760: for every
// stellar slot the loader left behind, set dominated from availability_flags
// 0x20 and seed Strength. Bodies carrying availability_flags 0x40 (Bible
// "starts the game destroyed")
// start at live strength -1 with the regeneration countdown pinned (1 when the
// schedule seed is negative, otherwise the schedule seed); every other body
// resets live strength to the loaded capacity with destroyed_days_remaining 0.
// Without this the loader's capacity seed would leave starts-destroyed bodies
// intact.
void ResetStellarStrengthForNewGame(GameState &state) {
  for (Stellar &stellar : state.scenario.stellars) {
    // Bible Flags2 0x20 "starts the game dominated"; the original sets the
    // persistent domination latch from this bit (Game_ResetNewGameState
    // 0x004b4690), not unconditionally false.
    stellar.dominated = (stellar.availability_flags & 0x20U) != 0U ? 1 : 0;
    if ((stellar.availability_flags & 0x40U) != 0U) {
      stellar.strength = -1;
      stellar.destroyed_days_remaining =
          stellar.schedule_days < 0 ? 1 : stellar.schedule_days;
    } else {
      stellar.strength = stellar.strength_capacity;
      stellar.destroyed_days_remaining = 0;
    }
  }
}

} // namespace

// Ghidra 0x004b3350 Ship_ResetPlayerShipState.
void NovaShip_ResetPlayerShipState(GameState &state) {
  // Ghidra 0x004b3350 does not overwrite g_player_ship_name. The selected
  // christening must survive this reset before the fresh pilot record is
  // materialized below.
  const std::string ship_name = state.player.ship_name;
  state.player = Ship{};
  state.player.ship_name = ship_name;
  state.player.pos_x = 50.0F;
  state.player.pos_y = 50.0F;
  state.player.is_active = true;
  state.player.ai_behavior_code = -1;
  state.player.travel_transfer_mode = -1;
  state.player.active_weapon_bank_slot = -1;
  state.player.death_timer_active = -999.0F;
  state.player.waypoint_arrival_marker_a = 1;

  state.inventory.cargo_bins.fill(0);
  state.inventory.outfit_owned_count.fill(0);
  state.inventory.junk_counts.fill(0);
  state.weapon_count_by_class.fill(0);
  state.weapon_secondary_count_by_class.fill(0);
  state.weapon_bank_cooldown.fill(0.0F);
  state.active_mission_runtime_flags = {};
  state.active_missions = {};
  state.active_shots.clear();

  state.player_stat_modifier_pct.fill(100);
  state.target_category_command.fill(-1);
  state.travel = {};
  state.travel.travel_hint_state = 0x7fff;
  state.game_over_pending = false;
  state.return_to_menu_pending = false;
  state.player_threat_active = false;
  state.player_threat_active_prev = false;
  state.pending_red_alert = false;
  state.bomb_detonation_timer = 0.0F;
  state.recently_hit_timer = 0.0F;

  NovaOutfit_RecomputeOutfitDerivedState(state);
  const PlayerEffectiveStats effective =
      Outfit_ComputePlayerEffectiveStats(state);
  state.player.shield_points = effective.max_shield_points;
  state.player.armor_points = effective.max_armor_points;
  state.player.fuel_points = effective.fuel_capacity;
}

// Test/entry seam for the Ghidra Game_ResetNewGameState (0x004b4690) stellar
// Strength/hazard reset, applied by the fresh-world flow.
void NovaNewPilot_ResetStellarStrengthForNewGame(GameState &state) {
  ResetStellarStrengthForNewGame(state);
}

bool NovaNewPilotFlow_Run(SdlPlatform &platform,
                          GameState &state,
                          bool ship_animations,
                          const std::function<void()> &render_background) {
  state.gameplay_now_ms = platform.gameplay_ticks_ms();
  // ---- Step 1: pilot naming/selection ------------------------------------
  // Ghidra: Menu_RunNewGameFlow prefills DAT_007d20b7/DAT_007d21b7 with random
  // STR# 0x80 sample names (done inside the dialog port) and runs
  // Menu_RunPilotSelectionDialog (0x0048a7e0), which fills the Full Name /
  // Nickname fields, the Strict Play flag, the Gender popup and the character
  // template; the port strips articles/subtitle suffixes inside the dialog
  // port, matching the original's post-accept strip.
  NovaFontCache font_cache;
  NewPilotDraft draft;
  if (!RunPilotSelectionDialog(
          platform, font_cache, state, draft, render_background)) {
    return false;
  }

  // ---- Step 2: overwrite-existing-pilot confirmation ----------------------
  // Ghidra Menu_RunNewGameFlow 0x00489d70 confirms the overwrite BEFORE the
  // ship christening: after the names are stripped it builds
  // <nova_files><Full Name>.plt and, when PilotFile_ProbeExists (0x004cd030)
  // hits, opens Ui_ShowConfirmDialog (0x004977d0, DLOG 0xbba) with STR# 0x8c
  // row 9; a declined prompt aborts the flow. The save directory must resolve
  // before any state is committed, so a missing directory aborts here too.
  const auto directory = PilotFileSaveDirectory();
  if (!directory) {
    NovaLog::Error("new pilot: no pilot save directory; aborting before any "
                   "state change");
    return false;
  }
  const auto new_pilot_path = *directory / (draft.first_name + ".plt");
  if (PilotFileProbeExists(new_pilot_path)) {
    const std::string prompt = NovaHud_LoadStringEntry(0x8c, 9).value_or(
        "A pilot with this name already exists. Replace it?");
    if (!NovaUi_ShowConfirmDialog(
            platform, font_cache, prompt, render_background)) {
      NovaLog::Info("new pilot: overwrite declined for '{}'",
                    new_pilot_path.string());
      return false;
    }
  }

  // The 0xc1e variant leaves the Character popup offscreen; the flow falls
  // back to the family's first entry (stock: the hidden .Trader).
  if (draft.character_template.empty()) {
    const auto templates = EnumeratePilotTemplates();
    if (!templates.empty()) {
      draft.character_template = templates.front().name;
    }
  }
  draft.start_type_code =
      ResolveStartTypeFromTemplate(draft.character_template);

  // ---- Step 3: ship christening -------------------------------------------
  // Ghidra: NovaUi_ShowTextConfirmCodeDialog (DLOG 0xbb9) with prompt =
  // STR# 0x7d2 row 0x79 + the start class's long name (DAT_005a9bcc table;
  // the port reads Ship::long_name from the scenario tables) and initial text
  // = a random STR# 0x80 row 7-9 ship name, max 0x40 chars. The result is
  // article-stripped into the draft ship name; the original writes the live
  // g_player_ship_name only in the success tail.
  {
    const std::string class_caption =
        state.scenario.Ship(
            static_cast<std::int16_t>(draft.start_type_code + 0x80))
            ? state.scenario
                  .Ship(static_cast<std::int16_t>(draft.start_type_code + 0x80))
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
    draft.ship_name = std::move(*ship_name);
  }

  // ---- Step 4: fresh-world reset ------------------------------------------
  // Every cancellable prompt has succeeded. Commit the draft's selections to
  // the live state only here; the original writes g_player_name /
  // g_player_ship_name / g_strict_play / the gender latch in
  // Menu_RunNewGameFlow's success tail (0x0048a51e onward). This keeps the
  // running pilot and the main-menu status panel unchanged when any prompt is
  // cancelled or quit before acceptance.
  if (platform.quit_requested()) {
    return false;
  }
  state.pilot.first_name = std::move(draft.first_name);
  state.pilot.last_name = std::move(draft.last_name);
  state.pilot.strict_play = draft.strict_play;
  state.pilot.male = draft.male;
  state.control.male = draft.male;
  state.pilot.character_template = std::move(draft.character_template);
  state.pilot.start_type_code = draft.start_type_code;
  state.player.ship_name = std::move(draft.ship_name);
  // Ghidra: g_travel_interaction_loop_active = 0, Ship_ResetPlayerShipState,
  // Game_ResetReputationAndAvailability/State, zero outfit/weapon tables,
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
  Stub_LoadScenarioResourceTables(state, ship_animations);
  // Ghidra Game_ResetNewGameState applies the starts-destroyed/hazard reset to
  // every stellar immediately after the tables load.
  ResetStellarStrengthForNewGame(state);
  ResetPlayerShipForNewGame(state);
  Stub_ResetReputationAndWorldTables(state);
  Stub_SeedStartingInventory(state);
  // Game_ResetNewGameState seeds the calendar from the local clock+250; the
  // selected character template then overrides it
  // (PilotData_InitializePlayerState 0x004cd4b0 param_2 != 0).
  SetNewGameDateAndStrings(state);
  // PilotData_InitializePlayerState 0x004cd4b0 (param_2 != 0): credits, combat
  // rating, the random System1-4 start-system pick, per-govt reputations and
  // the template start date. Runs before discovery so the picked system drives
  // the discovery/starmap/spawn seeding below. Returns the OnStart control-bit
  // set string, run after the world reset.
  const std::string on_start_script = ApplyCharacterTemplate(state);
  // System-dependent placement (reinforcement countdown, starmap pan origin,
  // spawn position) now that current_system_id is final.
  PlacePlayerInStartSystem(state);
  // Menu_RunNewGameFlow finalizes shield/armor/fuel only after the starting
  // outfit counts and weapon banks are seeded, so their max capacities account
  // for the starter loadout's outfit bonuses.
  RecomputePlayerMeters(state);
  Stub_DiscoverStartingSystems(state);

  // ---- Step 5: first travel destination + scenario spawn ------------------
  const std::int16_t first_save_stellar = PickFirstSaveStellar(state);
  // Ghidra 0x00489d70 calls Ship_DeactivateVacantShipsAndTally(1) before
  // rebuilding the initial population. With the new-game flag set, the
  // previous pilot's escorts are vacant too; leaving this call out lets them
  // leak into the new pilot and consume fleet slots/save rows.
  NovaShip_DeactivateVacantShipsAndTally(state, /*keep_player_engaged=*/true);
  // Ghidra 0x00489d70: Frame_TriggerSystemRegionEvents(current system)
  // 0x00467bd0, after the vacancy sweep and before the population rebuild. An
  // already-latched region fires nothing (each trigger is once-only), so this
  // does not duplicate the per-frame player-core call.
  NovaSystem_TriggerNebulaRegionEvents(state, state.player.current_system_id);
  // Ghidra: System_RebuildInitialNpcAndMissionPopulation(current, 1) +
  // System_UpdateSystemAndStellar display state + Asteroid_InitSystem (the
  // system's asteroid field). flag==1 maps to copy_player_heading=true in the
  // mission-fleet restore slice.
  NovaSystem_RestoreMissionFleets(state,
                                  state.player.current_system_id,
                                  /*copy_player_heading=*/true,
                                  platform.gameplay_ticks_ms());
  // First offering roll for the fresh world (Ship_InitGameplayDataTables
  // 0x00458802 arm also re-rolls on arrival; the loader zeroed the table).
  Mission_RerollOfferingRolls(state);
  NovaSystem_PopulateInitialNpcShips(state, state.player.current_system_id);
  // TODO(decomp(0x00489d70)) skipped scopes from Menu_RunNewGameFlow's
  // fresh-world tail, each with a known original call:
  //   - the stellar hazard-marker pass (availability flags 0x20/0x40 over all
  //     0x800 stellars)
  //   - the live date-block copy (g_current_game_year_month/day, DAT_00735460)
  //   - current-system field_0xc8/0xc4
  //   - per-ship zeroing of ionization_points/field_0xb0/
  //     turn_bank_animation_phase/ai_turn_bias_dir and DAT_007cab1c = 0xfffd
  //   - the second PilotData_InitializePlayerState pass (param 0) after the
  //     availability rolls

  // ---- Step 6: assemble the persistent pilot record and apply it ----------
  // Ghidra keeps the freshly-seeded pilot in a pilot-save block (resource id
  // 0x63688a72) created/accessed by PilotData_InitializePlayerState and
  // IntroCinematic_SetupFrames; the running game only materializes that
  // record on demand (see pilot_file.hpp). The reimplementation does the
  // same: build a PilotFile for the new pilot, then copy it into the live
  // GameState so the intro and spaceflight read one consistent record.
  PilotFile record = PilotFile::Fresh();
  record.pilot_name = state.pilot.first_name;
  record.nickname = state.pilot.last_name;
  // The christening result lives in g_player_ship_name/state.player until the
  // fresh pilot block is materialized. Carry it into the record before
  // PilotFileApply, otherwise the apply pass replaces it with an empty name.
  record.ship_name = state.player.ship_name;
  // A fresh pilot block is not a loaded save: PilotData_InitializePlayerState
  // leaves Ship_ResetPlayerShipState's live values in place, then the original
  // refills shield, armor and fuel after stock outfits are seeded. Our
  // in-memory record is applied below solely to keep the tracked intro/save
  // fields together, so it must carry those live values rather than its zero
  // initialization. Otherwise PilotFileApply would incorrectly erase the
  // newly filled ship and the seeded inventory.
  record.credits = state.player.credits;
  // The character template overrode the clock-seeded calendar; carry the
  // applied date and its prefix/suffix (block +0x134..+0x14a) so PilotFileApply
  // cannot reset them to Fresh()'s absent-block default.
  record.date = state.date;
  record.date_prefix = state.date_prefix;
  record.date_suffix = state.date_suffix;
  record.ship_class_id = state.player.ship_class_id;
  record.current_system_id = state.player.current_system_id;
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
  record.weapon_count_by_class = state.weapon_count_by_class;
  record.weapon_secondary_count_by_class =
      state.weapon_secondary_count_by_class;
  record.outfit_owned_count = state.inventory.outfit_owned_count;
  // Carry the reset control-bit table (Game_ResetNewGameState zeroes it, then
  // the clean-room ferry baseline b311 is re-seeded). Without this the fresh
  // record's all-zero block would clear the bits on PilotFileApply before the
  // character template's OnStart script runs. Mirrors PilotFileCollectFromState
  // (0x004c7dd0 saver).
  for (std::size_t i = 0; i < record.control_bits.size(); ++i) {
    const std::uint8_t persisted = state.control.persisted_bit_bytes[i];
    record.control_bits[i] = (persisted != 0) == state.control.bits.test(i)
                                 ? persisted
                             : state.control.bits.test(i) ? 1
                                                          : 0;
  }
  // Carry the starting legal record applied by ApplyCharacterTemplate;
  // PilotFileApply otherwise clears the whole reputation table to zero.
  const std::size_t reputation_count =
      std::min(state.system_reputation.size(), record.system_reputation.size());
  std::copy_n(state.system_reputation.begin(),
              reputation_count,
              record.system_reputation.begin());
  // Carry the start-system discovery seeded by Stub_DiscoverStartingSystems;
  // PilotFileApply otherwise copies the fresh record's all-zero fog table over
  // it (the fog consumers read discovery_state).
  const std::size_t discovery_count =
      std::min(state.scenario.systems.size(), record.system_discovery.size());
  for (std::size_t i = 0; i < discovery_count; ++i) {
    record.system_discovery[i] = state.scenario.systems[i].discovery_state;
  }

  // Carry the remaining live runtime fields the record now round-trips, so
  // PilotFileApply does not reset the new-pilot dialog selections
  // (strict_play/male) or the Game_ResetNewGameState stellar strength /
  // engagement seeding performed above.
  record.strict_play = state.pilot.strict_play;
  record.male = state.control.male;
  record.player_combat_rating_points = state.player_combat_rating_points;
  record.reinforcement_retrigger_delay = state.reinforcement_retrigger_delay;
  record.target_category_command = state.target_category_command;
  const std::size_t engage_count =
      std::min(state.scenario.stellars.size(),
               record.stellar_destroyed_days_remaining.size());
  for (std::size_t i = 0; i < engage_count; ++i) {
    record.stellar_destroyed_days_remaining[i] =
        state.scenario.stellars[i].destroyed_days_remaining;
  }
  // The loader marked every present përs resource active (+0x620); a fresh
  // record's pers flags are zero, so without this carry PilotFileApply would
  // clear the whole personality table (the tutorial derelict Viper included)
  // and the system-population pass would log "not present" and skip it.
  PilotFileSeedPersonalityPresence(state.scenario, record);

  // Copy the assembled record into the live state (mirroring the block-to-
  // global copy IntroCinematic_SetupFrames/PilotData_InitializePlayerState
  // perform). The intro and spaceflight modes read these live fields. The
  PilotFileApply(record, state);
  // Menu_RunNewGameFlow's state-reset tail (0x0048a600) ends with the
  // flight-hint state at -3 (0xfffd): the Ship_ResetPlayerShipState 0x7fff
  // latch from earlier in the bootstrap is overwritten, so a brand-new pilot
  // gets no launch departure message until the first hyperspace jump (the
  // flight-tutorial hints arm instead).
  state.travel.travel_hint_state = -3;
  // Ghidra 0x004cd3b0 IntroCinematic_SetupFrames: reads the keyed pilot block
  // (selected character template; absent block -> the no-save default) and
  // fills g_intro_cinematic, clamping ids/durations. Stock data: block
  // ".Trader" -> PICTs 0x2008/0x2009/0x200a for 45 ticks each, intro text
  // desc -1 (no dialog).
  NovaIntroCinematic_SetupFrames(state, state.pilot.character_template);
  NovaLog::Info("new-game intro configured: frames {} {} {} {} for {} {} {} {} "
                "ticks each (pilot block '{}', intro text desc {})",
                state.intro_cinematic.source_pict_ids[0],
                state.intro_cinematic.source_pict_ids[1],
                state.intro_cinematic.source_pict_ids[2],
                state.intro_cinematic.source_pict_ids[3],
                state.intro_cinematic.duration_60h_ticks[0],
                state.intro_cinematic.duration_60h_ticks[1],
                state.intro_cinematic.duration_60h_ticks[2],
                state.intro_cinematic.duration_60h_ticks[3],
                state.pilot.character_template,
                state.intro_cinematic.intro_text_desc_id);

  // ---- Step 7: mark active ------------------------------------------------
  // Ghidra: DAT_00596d28 = 1 (game active), g_strict_play = the dialog's
  // Strict Play checkbox state (latched by the dialog port into
  // state.pilot.strict_play).
  state.game_active = true;
  // Ghidra 0x00489d70 calls PilotData_InitializePlayerState(..., 0) here: it
  // runs the template's OnStart control-bit set script (block+0x32). Run it
  // before the save so any starting control bits are persisted.
  if (!on_start_script.empty()) {
    Mission_ExecuteReactionScript(
        state, on_start_script, MissionScriptContext{"OnStart", -1});
  }
  // Ghidra 0x00489d70 calls PilotFile_SaveGame after the fresh state and
  // character block have been finalized. The selected starting stellar is
  // the restore point persisted at block1+0x00. `directory` was resolved
  // before any prompt-confirmed state change.
  if (!PilotFileSaveGame(*directory, state, first_save_stellar)) {
    NovaLog::Error("new pilot: initial save failed for '{}'",
                   state.pilot.first_name);
  }
  NovaLog::Info("new pilot active: callsign '{}', start type {}",
                state.pilot.first_name,
                state.pilot.start_type_code);
  return true;
}

} // namespace game
