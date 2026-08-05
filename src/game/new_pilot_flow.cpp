#include "new_pilot_flow.hpp"

#include "../log.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <random>
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
    "the newcomer", "the wanderer", "the trader",  "the mercenary",
    "the courier",  "the smuggler", "the pilot",
};
constexpr std::array<std::string_view, 2> kStartTypeNames{
    "Default Start", "Alternate Start",
};

// ===========================================================================
// Modal prompt helpers
// ---------------------------------------------------------------------------
// The original performs all dialog interaction synchronously inside the flow
// (blocking modal loops). Reimplemented as small SDL frame loops that draw a
// centered cyan/border panel onto the current renderer with
// SDL_RenderDebugText and present it, mirroring DrawSplashFrame's approach in
// nova_app.cpp. These are intentionally minimal: they are not the real
// stock UI dialogs, which are a later milestone.

bool RunTextInputPrompt(SdlPlatform &platform, const std::string &prompt,
                        std::string initial, std::string &out) {
  SDL_Renderer *const renderer = platform.renderer();
  constexpr std::string_view kNavKeys = "Ctrl-BACKSPACE clears  /  ENTER accept  /  ESC cancel";
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
      const std::string line = std::to_string(i + 1) + "  " +
                              std::string(kStartTypeNames[i]);
      SDL_RenderDebugText(renderer, 130.0F,
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
  return static_cast<int>(std::uniform_int_distribution<int>{0, count - 1}(
      state.rng));
}

} // namespace

// ===========================================================================
// Step isolation: data subsystems not yet reconstructed. Each maps to a call
// in Menu_RunNewGameFlow and logs loudly what behaviour it does not reproduce,
// so the new-game path remains reachable end-to-end without silently faking
// the systems it depends on.
namespace {

void Stub_LoadScenarioResourceTables() {
  // Ghidra 0x004b9050 NovaData_LoadScenarioResourceTables reads the first
  // c\x9alr record plus the scenario table resources. Those tables drive
  // system/outfit/weapon class definitions, which are not reconstructed yet.
  // The player ship is left at the default class without scenario world data.
  NovaLog::Todo("scenario resource tables not loaded; player world uses "
                "fallback defaults");
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
  // list and weapon banks. Those ship/weapon class tables are not reconstructed
  // yet, so the arrays stay zero.
  state.outfit_owned_count.fill(0);
  state.weapon_bank_ammo.fill(0);
  state.weapon_bank_secondary.fill(0);
  NovaLog::Todo("starting inventory left empty: ship-class default outfit and "
                "weapon tables not reconstructed");
}

void Stub_DiscoverStartingSystems(GameState &state) {
  // Menu_RunNewGameFlow sets discovery_state = 1 on the starting system and
  // each visible neighbour so the starmap shows the pilot's immediate area.
  // SystemDef adjacency/discovery tables are not reconstructed yet.
  (void)state;
  NovaLog::Todo("starting-system discovery not applied: SystemDef tables and "
                "system-id registry not reconstructed");
}

void Stub_PickFirstTravelDestination(GameState &state) {
  // Ghidra Stellar_FindNearestAvailableTravelStellar picks the first adjacent,
  // visible system as the pilot's initial jump target (stored back into
  // state->travel by Stellar_SetTravelDestination when found).
  // System adjacency tables are not reconstructed yet, so no destination is
  // chosen; the in-game starmap later reports "no route".
  state.travel.selected_dest_id = -1;
  NovaLog::Todo("first travel destination not resolved: system adjacency "
                "table not reconstructed; starmap will show no route");
}

void ResetPlayerShipForNewGame(GameState &state) {
  // Ghidra 0x004b3350 Ship_ResetPlayerShipState: fresh position/velocity,
  // default class id, recomputed shield/armor/fuel, cleared targeting/travel/
  // mission/AI fields and debuffs.
  state.player.pos_x = state.player.pos_y = 0.0F;
  state.player.vel_x = state.player.vel_y = 0.0F;
  state.player.heading = 0.0F;
  state.player.speed = 0.0F;
  state.player.ship_class_id = 0; // default class; class table unavailable
  state.player.active_weapon_bank_slot = 0;
  state.player.timed_action_counter = -1;
  state.player.death_timer_active = -1.0F;
  state.player.is_active = true;
  // Shield/armor/fuel are recomputed from the class in the original; without
  // the class table they fall back to the template defaults (documented).
  state.player.shield_points = 0.0F;
  state.player.armor_points = 0.0F;
  state.player.fuel_points = 0.0F;
  NovaLog::Debug("player ship reset for new game");
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
  if (!RunTextInputPrompt(platform, "NEW PILOT  -  enter callsign:",
                          suggested, state.pilot.first_name)) {
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
  ResetPlayerShipForNewGame(state);
  Stub_ResetReputationAndWorldTables(state);
  Stub_SeedStartingInventory(state);
  Stub_DiscoverStartingSystems(state);
  Stub_LoadScenarioResourceTables();
  SetNewGameDateAndStrings(state);

  // ---- Step 5: first travel destination + scenario spawn ------------------
  Stub_PickFirstTravelDestination(state);
  // Ghidra: Mission_SpawnSystemMisnShips + System_UpdateSystemAndStellar
  // display state + System_InitRoamingShips. Mission/system definitions are
  // not reconstructed; skipped.
  NovaLog::Todo("system scenario ships and roamer population not spawned: "
                "mission and ship tables not reconstructed");

  // ---- Step 6: intro cinematic configuration ------------------------------
  // Ghidra: IntroCinematic_SetupFrames fills g_intro_cinematic. Without the
  // pilot save block (0x63688a72) it defaults to a single PICT 0x2008 shown
  // for 10 ticks. That default is what the reimplementation uses.
  state.intro_cinematic = IntroCinematicData{};
  state.intro_cinematic.source_pict_ids = {0x2008, -1, -1, -1};
  state.intro_cinematic.duration_60h_ticks = {10, 0, 0, 0};
  NovaLog::Info("new-game intro configured: single PICT 0x2008 for 10 ticks "
                "(no pilot save block)");

  // ---- Step 7: mark active ------------------------------------------------
  // Ghidra: DAT_00596d28 = 1 (game active), DAT_00596d2f = repoChoice.
  state.pilot.selected_reputation = static_cast<std::int16_t>(start_type);
  state.game_active = true;
  NovaLog::Info("new pilot active: callsign '{}', start type {}",
                state.pilot.first_name, state.pilot.start_type_code);
  return true;
}

} // namespace game
