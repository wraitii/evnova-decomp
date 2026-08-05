#include "new_pilot_flow.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "pilot_file.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace game {
namespace {

// The original builds these from string-table entries (Resource_LoadStringEntry
// with ids 0x80..). Rather than depend on the string-table resource format
// (not reconstructed), the reimplementation sources the same wording inline
// and logs the divergence. The click-backed full-screen context menu and text
// confirm dialog also predate the current framework, so they are reduced to
// small modal prompts with the same semantics (see below).
constexpr std::array<std::string_view, 7> kOpenerFirstNames{
    "the newcomer",
    "the wanderer",
    "the trader",
    "the mercenary",
    "the courier",
    "the smuggler",
    "the pilot",
};
constexpr std::array<std::string_view, 2> kStartTypeNames{
    "Default Start",
    "Alternate Start",
};

// Where a brand-new pilot begins. The real game randomizes the start system
// among a few candidates at PilotData_InitializePlayerState (fresh-seed picks a
// random valid starting system from the pilot-block's four stored choices); we
// hardcode one (Tichel, zero-based 1) for now so the in-flight view has a known
// landmark. TODO(decomp): implement the randomized start-system selection once
// the pilot-block start candidates are decoded.
constexpr std::int16_t kStartSystemId = 1;            // zero-based (Tichel)
constexpr std::int16_t kStartSystemResourceId = 0x81; // resource id

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
// Modal prompt helpers
// ---------------------------------------------------------------------------
// The original performs all dialog interaction synchronously inside the flow
// (blocking modal loops). Reimplemented as small SDL frame loops that draw a
// centered cyan/border panel onto the current renderer with
// SDL_RenderDebugText and present it, mirroring DrawSplashFrame's approach in
// nova_app.cpp. These are intentionally minimal: they are not the real
// stock UI dialogs, which are a later milestone.

bool RunTextInputPrompt(SdlPlatform &platform,
                        const std::string &prompt,
                        std::string initial,
                        std::string &out) {
  SDL_Renderer *const renderer = platform.renderer();
  constexpr std::string_view kNavKeys =
      "Ctrl-BACKSPACE clears  /  ENTER accept  /  ESC cancel";
  constexpr std::size_t kMaxChars = 48;

  out = std::move(initial);
  bool running = true;
  while (running && !platform.quit_requested()) {
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      switch (input->key) {
      case TextKey::enter:
        running = false; // accept
        break;
      case TextKey::escape:
        return false;
      case TextKey::backspace:
        if (!out.empty()) {
          out.pop_back();
        }
        break;
      case TextKey::character:
        if (out.size() < kMaxChars) {
          out.push_back(input->character);
        }
        break;
      case TextKey::none:
      case TextKey::primary: // mouse click does not edit a callsign
        break;
      }
    }

    SDL_SetRenderDrawColor(renderer, 1, 4, 12, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 48, 113, 179, SDL_ALPHA_OPAQUE);
    const SDL_FRect panel{96.0F, 142.0F, 448.0F, 150.0F};
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 142, 209, 255, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 202, 224, 255, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugText(renderer, 130.0F, 160.0F, prompt.c_str());
    const std::string shown = "> " + out + "_";
    SDL_RenderDebugText(renderer, 130.0F, 200.0F, shown.c_str());
    SDL_SetRenderDrawColor(renderer, 128, 170, 210, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugText(renderer, 130.0F, 240.0F, kNavKeys.data());
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
  return true;
}

// Returns 0..2 for the start type, or -1 on cancel. Mirrors the original
// menu-based pilot-selection step, reduced to a modality prompt.
int RunStartTypePrompt(SdlPlatform &platform) {
  SDL_Renderer *const renderer = platform.renderer();
  constexpr std::string_view kNavKeys = "  1 / 2 select  /  ESC cancel";
  while (!platform.quit_requested()) {
    for (std::optional<TextInput> input; (input = platform.PollTextEvent());) {
      if (input->key == TextKey::character && input->character == '1') {
        return 0;
      }
      if (input->key == TextKey::character && input->character == '2') {
        return 1;
      }
      if (input->key == TextKey::escape) {
        return -1;
      }
      // TextKey::primary / none: mouse click does not select a start type.
    }
    SDL_SetRenderDrawColor(renderer, 1, 4, 12, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 48, 113, 179, SDL_ALPHA_OPAQUE);
    const SDL_FRect panel{96.0F, 142.0F, 448.0F, 160.0F};
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 142, 209, 255, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 202, 224, 255, SDL_ALPHA_OPAQUE);
    for (std::size_t i = 0; i < kStartTypeNames.size(); ++i) {
      const std::string line =
          std::to_string(i + 1) + "  " + std::string(kStartTypeNames[i]);
      SDL_RenderDebugText(renderer,
                          130.0F,
                          174.0F + static_cast<float>(i) * 34.0F,
                          line.c_str());
    }
    SDL_SetRenderDrawColor(renderer, 128, 170, 210, SDL_ALPHA_OPAQUE);
    SDL_RenderDebugText(renderer, 130.0F, 262.0F, kNavKeys.data());
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
  return -1;
}

[[nodiscard]] int RandomIndex(GameState &state, int count) {
  return static_cast<int>(
      std::uniform_int_distribution<int>{0, count - 1}(state.rng));
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
  // sp\x9ab stellars, s\xd8st systems) and rebuilds the global class tables.
  // The reimplementation does the same into state.scenario; the player ship
  // can now read its real class stats here.
  if (!state.scenario.LoadFromArchives()) {
    NovaLog::Todo("scenario resource tables could not be loaded; player world "
                  "uses fallback defaults");
  }
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
  // list (DefaultItems) and stock weapon banks. The ship-class tables are now
  // available in state.scenario, so for the default ship (id 0x80, zero-based
  // 0) the outfit counts are populated from its default items.
  state.outfit_owned_count.fill(0);
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
    if (index >= state.outfit_owned_count.size()) {
      continue;
    }
    state.outfit_owned_count[index] =
        static_cast<std::int16_t>(ship->default_outfit_counts[i]);
  }
  NovaLog::Info("new-game inventory seeded from ship class '{}' ({} default "
                "outfits)",
                ship->display_name,
                std::count_if(ship->default_outfit_ids.begin(),
                              ship->default_outfit_ids.end(),
                              [](std::int16_t id) { return id >= 0x80; }));
}

void Stub_DiscoverStartingSystems(GameState &state) {
  // Menu_RunNewGameFlow sets discovery_state = 1 on the starting system and
  // each adjacent neighbour so the starmap shows the pilot's immediate area.
  // The starting system is the pilot's entry point. The reimplementation
  // tracks discovered systems by id in GameState instead of the original
  // SystemDef discovery bits; the starmap later reads it.
  (void)state;
  // TODO(decomp): mark the starting system (kStartSystemResourceId) and each
  // linked neighbour discovered once a discovery-container is added to
  // GameState. The system links are now readable via
  // state.scenario.System(kStartSystemResourceId)->links.
  NovaLog::Info("starting system discovery pending: system adjacency now "
                "available via scenario tables");
}

void Stub_PickFirstTravelDestination(GameState &state) {
  // Ghidra Stellar_FindNearestAvailableTravelStellar picks the first adjacent,
  // reachable system as the pilot's initial jump target, stored back into
  // state->travel by Stellar_SetTravelDestination when found. Here the first
  // live link of the starting system (kStartSystemResourceId) stands in for
  // that choice. Ghidra zero-based link ids: -1 or < 0x80 is unused; links
  // >= 0x80 name a system (the loader re-bases them into the 0.. index space
  // after a sanity range check, so we keep them as resource ids here).
  state.travel.selected_dest_id = -1;
  if (const auto *start = state.scenario.System(kStartSystemResourceId);
      start) {
    for (const auto link : start->links) {
      if (link >= 0x80) {
        state.travel.selected_dest_id = link;
        break;
      }
    }
  }
  if (state.travel.selected_dest_id >= 0) {
    NovaLog::Info("initial travel destination resolved to system id {} from "
                  "starting-system links",
                  state.travel.selected_dest_id);
  } else {
    NovaLog::Todo("first travel destination not resolved: starting system has "
                  "no outward link defined; starmap will show no route");
  }
}

void ResetPlayerShipForNewGame(GameState &state) {
  // Ghidra 0x004b3350 Ship_ResetPlayerShipState: fresh position/velocity,
  // default class id, recomputed shield/armor/fuel, cleared targeting/travel/
  // mission/AI fields and debuffs.
  //
  // Start in the hardcoded starting system (Tichel for now; the original
  // randomizes among a few start candidates). The world coordinate axes follow
  // the game convention confirmed from the flight renderer: heading 0 = up
  // (-y), so world +y is down on screen (Math_AddPolarVelocity projects
  // heading -> vel = (sin, -cos); pos += vel * dt).
  state.player.current_system_id = kStartSystemId;

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
  state.player.ship_class_id = 0; // default class (ship id 0x80)
  state.player.active_weapon_bank_slot = 0;
  state.player.timed_action_counter = -1;
  state.player.death_timer_active = -1.0F;
  state.player.is_active = true;
  // Shield/armor/fuel are recomputed from the default ship class (Ghidra
  // multiplies the class base values by x5 for shields/armor per the negative
  // held/Shield convention; here we take the raw base values). Falls back to
  // the template defaults when the class table is unavailable.
  if (const auto *ship = state.scenario.Ship(0x80)) {
    state.player.shield_points = static_cast<float>(ship->base_shield);
    state.player.armor_points = static_cast<float>(ship->base_armor);
    state.player.fuel_points = static_cast<float>(ship->base_fuel);
    NovaLog::Debug("player ship reset for new game: class '{}' (holds {}, "
                   "shield {}, armor {}, fuel {})",
                   ship->display_name,
                   ship->cargo_holds,
                   ship->base_shield,
                   ship->base_armor,
                   ship->base_fuel);
  } else {
    state.player.shield_points = 0.0F;
    state.player.armor_points = 0.0F;
    state.player.fuel_points = 0.0F;
    NovaLog::Todo("player ship reset without the default ship class table");
  }
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

bool NovaNewPilotFlow_Run(SdlPlatform &platform, GameState &state) {
  // ---- Step 1: random opener strings + pilot naming/selection ------------
  // Ghidra: NovaRandom_Range(3) twice, Resource_LoadStringEntry into
  // DAT_007d20b7/DAT_007d21b7, then Menu_RunPilotSelectionDialog which fills
  // the pilot's first/last name. Strips articles/subtitles afterwards.
  // Replaced opener-source with kOpenerFirstNames; selection with
  // RunTextInputPrompt.
  // Ghidra picks a random opener string to prefill the callsign; kOpenerFirst
  // names stand in for the string-table entries and seed the suggestion.
  state.pilot.first_name.clear();
  const std::string suggested = std::string(
      kOpenerFirstNames[static_cast<std::size_t>(RandomIndex(state, 7))]);
  if (!RunTextInputPrompt(platform,
                          "NEW PILOT  -  enter callsign:",
                          suggested,
                          state.pilot.first_name)) {
    return false;
  }
  if (state.pilot.first_name.empty()) {
    NovaLog::Warn("new pilot cancelled: empty callsign");
    return false;
  }

  // ---- Step 2: start-type selection ---------------------------------------
  // Ghidra: PilotData_ResolveStartType (optional for mods/older configs) then
  // a text-confirm-code dialog the player must accept. Reduced to a modal
  // pick of the start-type row.
  const int start_type = RunStartTypePrompt(platform);
  if (start_type < 0) {
    return false;
  }
  state.pilot.start_type_code = static_cast<std::int16_t>(start_type);

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
  // Ghidra loads the scenario data tables (ships/outfits/weapons/stellars/
  // systems) as part of the fresh world reset; the ship reset and inventory
  // seed below read class stats, so load the tables first.
  Stub_LoadScenarioResourceTables(state);
  ResetPlayerShipForNewGame(state);
  Stub_ResetReputationAndWorldTables(state);
  Stub_SeedStartingInventory(state);
  Stub_DiscoverStartingSystems(state);
  SetNewGameDateAndStrings(state);

  // ---- Step 5: first travel destination + scenario spawn ------------------
  Stub_PickFirstTravelDestination(state);
  // Ghidra: Mission_SpawnSystemMisnShips + System_UpdateSystemAndStellar
  // display state + System_InitRoamingShips. Mission/system definitions are
  // not reconstructed; skipped.
  NovaLog::Todo("system scenario ships and roamer population not spawned: "
                "mission and ship tables not reconstructed");

  // ---- Step 6: assemble the persistent pilot record and apply it ----------
  // Ghidra keeps the freshly-seeded pilot in a pilot-save block (resource id
  // 0x63688a72) created/accessed by PilotData_InitializePlayerState and
  // IntroCinematic_SetupFrames; the running game only materializes that
  // record on demand (see pilot_file.hpp). The reimplementation does the
  // same: build a PilotFile for the new pilot, then copy it into the live
  // GameState so the intro and spaceflight read one consistent record.
  PilotFile record = PilotFile::Fresh();
  record.pilot_name = state.pilot.first_name;
  record.current_system_id = state.player.current_system_id;

  // IntroCinematic_SetupFrames reads the intro frames from the pilot-save
  // block (+0x20 / +0x28 / +0x30). Here the record is seeded from the default
  // character (ch\x9ar) resource's IntroPict1-4 / PictDelay1-4 fields: the
  // stock .Trader pilot uses IntroPict 0x2008/0x2009/0x200a for 45 1/60s ticks
  // each. post_intro_dest_id stays at the no-save default 0x7ffd ("no stellar
  // yet", but != -1 so the destination dialog still opens).
  if (const auto char_intro = NovaResource_LoadCharacterIntro()) {
    record.intro_source_pict_ids = char_intro->pict_ids;
    record.intro_duration_60h_ticks = char_intro->delay_ticks;
  } else {
    // IntroCinematic_SetupFrames' own no-save fallback: a single PICT 0x2008
    // shown for 10 ticks.
    record.intro_source_pict_ids = {0x2008, -1, -1, -1};
    record.intro_duration_60h_ticks = {10, 0, 0, 0};
  }
  record.post_intro_dest_id = 0x7ffd;

  // Copy the assembled record into the live state (mirroring the block-to-
  // global copy IntroCinematic_SetupFrames/PilotData_InitializePlayerState
  // perform). The intro and spaceflight modes read these live fields. The
  // record is kept in memory only (no .plt writer); see PilotFileApply.
  PilotFileApply(record, state);
  NovaLog::Info("new-game intro configured: frames {} {} {} {} for {} {} {} "
                "ticks each (character resource)",
                state.intro_cinematic.source_pict_ids[0],
                state.intro_cinematic.source_pict_ids[1],
                state.intro_cinematic.source_pict_ids[2],
                state.intro_cinematic.source_pict_ids[3],
                state.intro_cinematic.duration_60h_ticks[0],
                state.intro_cinematic.duration_60h_ticks[1],
                state.intro_cinematic.duration_60h_ticks[2]);

  // ---- Step 7: mark active ------------------------------------------------
  // Ghidra: DAT_00596d28 = 1 (game active), DAT_00596d2f = repoChoice.
  state.pilot.selected_reputation = static_cast<std::int16_t>(start_type);
  state.game_active = true;
  NovaLog::Info("new pilot active: callsign '{}', start type {}",
                state.pilot.first_name,
                state.pilot.start_type_code);
  return true;
}

} // namespace game
