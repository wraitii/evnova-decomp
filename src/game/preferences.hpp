#pragma once

// Clean-room model of the game's preferences (the main-menu Settings dialog,
// DLOG 0xfa3) and the per-pilot key-binding table. The original keeps these
// in a swath of globals (g_pref_intro_music, g_pref_sound_volume, ..., and
// g_player_key_bindings); here they are one value object so the
// reimplementation stays free of hidden globals and singletons (AGENTS.md).
// Each field names its Ghidra global in a comment.
//
// Mirrors NovaPrefs_ResetToDefaults (0x004b4320) and the Settings dialog
// Menu_RunSettingsDialog (0x00488650). See docs/preferences_keybindings.md for
// the authoritative item map and the direction (inverted-ness) of each toggle.

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

class SdlAudio;
class SdlMusic;
class SdlPlatform;

namespace game {

// game::NovaFontCache (nova_font.hpp) — forward-declared here so the header
// shows the dialog runner without pulling SDL_ttf into every consumer.
class NovaFontCache;

// game::NovaExtraPrefs (preferences_extra.hpp) — port-only settings with no
// slot in the original .prf payload. Forward-declared so this header does not
// pull the INI layer into every consumer.
struct NovaExtraPrefs;

// The gameplay command -> key-code binding table. Slot index == command id
// (confirmed by the Ship_HandlePlayerShipControl reads); the stored value is
// the game's normalized physical-key code. Most codes retain PC set-1/DIK
// numbering; navigation/right-modifier keys occupy its compact 0x60..0x6f
// range. 0xff = unbound. Mirrors g_player_key_bindings
// (0x005914e6, short[0x52]) and NovaPrefs_ResetKeyBindings (0x004b4400).
struct KeyBindings {
  static constexpr std::size_t kSlotCount = 0x52;
  std::array<std::uint16_t, kSlotCount> cmd_to_key{};

  // Defaults exactly as NovaPrefs_ResetKeyBindings writes them.
  void ResetToDefaults();
};

// Ghidra String_ExpandControlCode (0x004f1990): the display name the Escape-
// code map (PTR_s_Escape_005776dc) substitutes for a normalized physical-key
// code, or "none" when the key is unbound. Exposed for the flight-tutorial
// hint text (STR# 0x7d2 entries 0x18-0x1c) which embeds the land/map/jump
// binding names.
[[nodiscard]] std::string NovaPrefs_KeyCodeDisplayName(std::uint16_t key_code);

// One stored preference value. The original keeps 8-bit toggled bytes; here
// they are bools. The "inverted" Mac-era flags (a 0 value means the feature is
// ON) are handled where the box is drawn/read, not here: the struct value is
// the *global bit* the original stores (so quicktime_movies == 0 means the
// QuickTime movies feature is enabled).
struct NovaPreferences {
  KeyBindings bindings;

  // Ghidra g_pref_intro_music. When off the Settings OK path stops the menu
  // music (the original's menu bass is the "intro" music here).
  bool intro_music = true;
  // Ghidra g_pref_sound_volume, 0..8. The on-screen word is STR# 0x88 row
  // `volume+1` (0 = "No Bloody Noise" ... 8 = "Hoo Boy!").
  std::int32_t sound_volume = 5;
  // Ghidra g_pref_brightness, 0..6.
  std::int32_t brightness = 3;

  // Ghidra g_pref_share_processor_time.
  bool share_processor_time = true;
  // Ghidra g_pref_quicktime_movies (inverted flag: 0 = QuickTime on). Locked
  // to 1 ("off") by NovaPrefs_ApplyLockedPreferences: the port has no
  // QuickTime movie playback, so a legacy .prf cannot enable it.
  bool quicktime_movies = true;
  // Ghidra g_pref_smoke_trails (inverted flag: 0 = smoke trails on).
  bool smoke_trails = false;
  // Ghidra DAT_00bec178 (run in a window), toggled live via the window mode.
  // The port defaults to a window (the original defaulted to fullscreen); the
  // checkbox is live and applies the OS window mode through SdlPlatform.
  bool run_in_window = true;
  // Ghidra g_pref_ship_animations.
  bool ship_animations = true;
  // Ghidra g_pref_engine_glows.
  bool engine_glows = true;
  // Ghidra g_pref_running_lights (defaults to the same bit as weapon_effects).
  bool running_lights = true;
  // Ghidra g_pref_weapon_effects.
  bool weapon_effects = true;
  // Ghidra g_pref_parallax_starfield.
  bool parallax_starfield = true;
  // Ghidra g_pref_ambient_sounds.
  bool ambient_sounds = true;
  // Ghidra g_hyperspace_effects (inverted flag: 0 = effects on). Used by the
  // hyperspace flash as the CE colour gate: non-zero forces black, otherwise
  // the requested white is applied (0x00872384). The Settings checkbox reads
  // inverted(hyperspace_effects), so the default (false) is effects-on white.
  // The CE build also uses this byte as a raw-input lock; the clean-room keeps
  // it as a pure effect toggle (documented divergence).
  bool hyperspace_effects = false;
  // Ghidra g_pref_check_for_updates (inverted flag: 0 = check for updates).
  bool check_for_updates = true;

  // CE addition persisted at .prf +0x76. It is not exposed by DLOG 0xfa3:
  // the galaxy starmap's Show/Hide Borders button toggles it and it is
  // persisted at the normal .prf save points. The original defaults it OFF and
  // its overlay was slow/buggy; the port's overlay is cheap, so it defaults ON
  // (see GameState::starmap_show_borders, the runtime copy the map edits).
  bool starmap_show_borders = true;

  // Populates every field (and the key table) with the original defaults.
  void ResetToDefaults();
};

// Forces the quality/graphics preferences the SDL port always renders. The
// Settings dialog shows them disabled and a legacy .prf cannot turn them off;
// call this after NovaPrefs_LoadFromFile (and it is applied by
// ResetToDefaults). Locked: share_processor_time=true, quicktime_movies=true
// (inverted: 1 = movies off; playback unsupported), smoke_trails=false,
// ship_animations=true,
// engine_glows=true, running_lights=true, weapon_effects=true,
// parallax_starfield=true, check_for_updates=true, brightness=3.
// hyperspace_effects, starmap_show_borders and run_in_window are NOT locked:
// the first is a live effect toggle, the map owns the second, and the third
// defaults ON with the Settings checkbox applying it live.
void NovaPrefs_ApplyLockedPreferences(NovaPreferences &prefs);

// @port 0x00872384 100%
// Ghidra 0x00872384 DrawContext_SetHyperspaceFlashColor.
// Resolves the hyperspace flash colour the CE build applies at the jump and
// hypergate arrival flashes: the raw g_hyperspace_effects byte non-zero forces
// black, otherwise the requested white is applied. The stored byte is inverted
// relative to the Settings checkbox, so the default (false) is effects-on
// white. The port has no DrawContext; the spaceflight loop resolves this into
// GameState.screen_flash_black and the view draws the full-screen overlay.
[[nodiscard]] constexpr bool
NovaPrefs_HyperspaceFlashIsBlack(const NovaPreferences &prefs) {
  return prefs.hyperspace_effects;
}

// @port 0x0046ab60 100%
// Ghidra 0x0046ab60 NovaAudio_UpdateCenteredGainFromPreference.
// The original first maps the stored preference to an integer mixer level
// (sound_volume * 8, clamped to 0x100). Audio_AllocateVoiceSlot then clamps
// each channel to 0x80, and the backend applies level * sample >> 7. Express
// that complete path as the normalized gain expected by SDL.
[[nodiscard]] constexpr float
NovaAudio_EffectGainFromPreference(std::int32_t sound_volume) {
  const std::int32_t centered_level =
      sound_volume < 0 ? 0 : (sound_volume > 32 ? 0x100 : sound_volume * 8);
  const std::int32_t voice_level =
      centered_level > 0x80 ? 0x80 : centered_level;
  return static_cast<float>(voice_level) / 128.0F;
}

// Ghidra 0x004c7400 NovaPrefs_LoadOrInit / 0x004c7820
// NovaPrefs_SaveToDisk. Path-taking forms expose the original 0x8c-byte,
// version-0x69 format for tests. System forms use SDL_GetPrefPath.
[[nodiscard]] bool NovaPrefs_LoadFromFile(const std::filesystem::path &path,
                                          NovaPreferences &prefs);
[[nodiscard]] bool NovaPrefs_SaveToFile(const std::filesystem::path &path,
                                        const NovaPreferences &prefs);
[[nodiscard]] bool NovaPrefs_LoadFromSystemStore(NovaPreferences &prefs);
[[nodiscard]] bool NovaPrefs_SaveToSystemStore(const NovaPreferences &prefs);

// Runs the main-menu Settings dialog (DLOG 0xfa3) as a blocking modal over the
// playfield, editing `prefs` in place. Mirrors Menu_RunSettingsDialog
// (0x00488650): the OK button commits (the caller persists the .prf) and
// returns true; Esc/Cancel discards and returns false. The Key Settings button
// opens the rebinding modal (Menu_RunKeySettingsDialog). Intro-music toggling
// stops/restarts `music` and windowed mode is applied to `platform` live, so
// the prefs feel immediate like the original. Returns true when the player
// pressed OK.
//
// Port-only addition: a synthetic "Extra Prefs" button opens
// NovaMenu_RunExtraPrefsDialog (preferences_extra.hpp) on `extra_prefs`, which
// edits the port-only presentation multipliers and bug-fix policy. The
// original dialog has no such control; that button is the only extra-prefs
// code left in this translation unit.
bool NovaMenu_RunSettingsDialog(
    SdlPlatform &platform,
    SdlAudio &audio,
    SdlMusic &music,
    NovaFontCache &font_cache,
    NovaPreferences &prefs,
    NovaExtraPrefs &extra_prefs,
    const std::function<void()> &render_background = {});

// Runs the separate Key Settings modal (Ghidra
// Menu_RunKeySettingsDialog, 0x0048b280). The visible rows edit a shadow copy
// of the 34 command bindings; Cancel discards it, Set Default resets it, and
// OK copies it back only when no duplicate key is present.
bool NovaMenu_RunKeySettingsDialog(
    SdlPlatform &platform,
    NovaFontCache &font_cache,
    NovaPreferences &prefs,
    const std::function<void()> &render_background = {});

} // namespace game
