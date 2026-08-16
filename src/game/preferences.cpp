#include "preferences.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../sdl_audio.hpp"
#include "../sdl_music.hpp"
#include "../sdl_platform.hpp"
#include "nova_font.hpp"

#include <SDL3/SDL_render.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace game {
namespace {

// ---------------------------------------------------------------------------
// Preferences dialog (Ghidra Menu_RunSettingsDialog, 0x00488650).
//
// DLOG 0xfa3 is a 336x296 window centred at (152,92) on the 640x480 playfield.
// Its DITL carries the option checkboxes, the two slider stacks, the OK and
// Key Settings buttons, and the label/value boxes. Item ordinals (1-based)
// and the direction of each toggle are documented in
// docs/preferences_keybindings.md and verified against the decompile.
//
// Control ordinals (1-based) -> NovaDialogItem::index (0-based):
//   OK=1/0, Share=2/1, sound label=4/3, sound value=5/4, sound down/up=6/7
//   /5/6, Intro=8/7, QuickTime=9/8, Smoke=10/9, window=11/10, ShipAnim=12/11,
//   Engine=13/12, Running=14/13, Weapon=15/14, KeySettings=16/15,
//   Parallax=18/17, Ambient=20/19, Hyperspace=21/20, CheckUpdates=22/21,
//   brightness label=23/22, brightness value=24/23, brightness down/up=25/26
//   /24/25.
constexpr std::uint16_t kSettingsDialogId = 0xfa3;
// STR# holding the nine sound-volume words, indexed by volume+1 (index 0..8).
constexpr std::uint16_t kSoundVolumeString = 0x88;

// Dialog chrome colours (clean-room stand-in for the original window; the
// DLOG has no backdrop PICT documented, so a flat bordered panel is drawn).
constexpr SDL_Color kPanel{12, 24, 44, 255};
constexpr SDL_Color kBorder{96, 148, 200, 255};
constexpr SDL_Color kTitle{230, 232, 240, 255};
constexpr SDL_Color kBody{200, 212, 226, 255};
constexpr SDL_Color kDim{136, 158, 184, 255};

// Returns the STR# 0x88 sound-volume word for the given volume index, or the
// original's fallback string DAT_0056cf0c (" ") when out of range. The value
// box text mirrors UiPanel_SetEntryTextPascal.
[[nodiscard]] std::string LoadSoundVolumeWord(std::int32_t volume) {
  if (volume < 0 || volume > 8) {
    return " ";
  }
  const auto data =
      NovaResource_Load(kResourceTypeStringTable, kSoundVolumeString);
  if (!data || data->size() < 2) {
    NovaLog::Todo("STR# 0x88 (sound-volume words) unavailable for the "
                  "Settings dialog");
    return " ";
  }
  // STR# payload: BE u16 count, then count Pascal strings.
  const std::size_t count = static_cast<std::size_t>((*data)[0]) << 8U |
                            static_cast<std::size_t>((*data)[1]);
  std::size_t pos = 2;
  for (std::size_t i = 0; i < count && pos < data->size(); ++i) {
    const std::size_t len = static_cast<std::size_t>((*data)[pos]);
    if (i == static_cast<std::size_t>(volume)) {
      if (pos + 1 + len <= data->size()) {
        return std::string(
            reinterpret_cast<const char *>(data->data() + pos + 1), len);
      }
      break;
    }
    pos += 1 + len;
  }
  return " ";
}

// Centre a window box (def->right-left x def->bottom-top) on the playfield,
// truncating the half-offset toward zero to match Dialog_CreateFromDlog
// (FUN_008730a1) integer arithmetic.
[[nodiscard]] SDL_FRect
CenterWindowOnPanel(const SDL_FRect &panel, float win_w, float win_h) {
  const float x = panel.x + std::truncf((panel.w - win_w) / 2.0F);
  const float y = panel.y + std::truncf((panel.h - win_h) / 2.0F);
  return SDL_FRect{x, y, win_w, win_h};
}

// Maps one DITL item rect (top,left,bottom,right in the window's own space)
// to an SDL_FRect in playfield coordinates by adding the window origin.
[[nodiscard]] SDL_FRect ItemRect(const NovaDialogItem &item,
                                 const SDL_FRect &win) {
  const SDL_FRect r{win.x + static_cast<float>(item.left),
                    win.y + static_cast<float>(item.top),
                    static_cast<float>(item.right - item.left),
                    static_cast<float>(item.bottom - item.top)};
  return r;
}

[[nodiscard]] bool Contains(const SDL_FRect &r, float x, float y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Draws a small checkbox glyph (a hollow square, filled when checked) centred
// in `box`, then its label text to the right.
void DrawCheckBox(SdlPlatform &platform,
                  NovaFontCache &font_cache,
                  const SDL_FRect &box,
                  std::string_view label,
                  bool checked) {
  SDL_Renderer *renderer = platform.renderer();
  const float size = 10.0F;
  const SDL_FRect glyph{box.x, box.y + (box.h - size) / 2.0F, size, size};
  SDL_SetRenderDrawColor(
      renderer, kBorder.r, kBorder.g, kBorder.b, SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &glyph);
  if (checked) {
    SDL_RenderFillRect(renderer, &glyph);
  }
  const float baseline = box.y + box.h / 2.0F + 4.0F;
  NovaText_Draw(platform,
                font_cache,
                NovaFontFamily::kGeneva,
                12.0F,
                kNovaFontStyleRegular,
                kBody,
                box.x + 14.0F,
                baseline,
                label);
}

// Draws one of the small slider arrows (pointing up in the upper cell, down in
// the lower), as a small filled triangle to keep it font-independent.
void DrawSliderArrow(SdlPlatform &platform,
                     NovaFontCache &,
                     const SDL_FRect &box,
                     bool up) {
  const float cx = box.x + box.w / 2.0F;
  const float cy = box.y + box.h / 2.0F;
  const float base = 4.0F; // halfwidth of the triangle base
  const float h = 6.0F;    // triangle height
  const float top_y = up ? cy - h / 2.0F : cy + h / 2.0F;
  // Fill the triangle as horizontal strips, thinnest at the tip, widest at the
  // base.
  SDL_SetRenderDrawColor(
      platform.renderer(), kBody.r, kBody.g, kBody.b, SDL_ALPHA_OPAQUE);
  const int rows = 4;
  for (int i = 0; i < rows; ++i) {
    const float t =
        (up ? static_cast<float>(i) : static_cast<float>(rows - 1 - i)) /
        static_cast<float>(rows - 1);
    const float yy =
        up ? top_y + static_cast<float>(i) : top_y - static_cast<float>(i);
    const float half = base * t;
    const float x0 = std::max(cx - half, box.x);
    const float x1 = std::min(cx + half, box.x + box.w);
    if (x1 > x0) {
      const SDL_FRect strip{x0, yy, x1 - x0, 1.0F};
      SDL_RenderFillRect(platform.renderer(), &strip);
    }
  }
}

// Draws a bordered push button with its centred label.
void DrawButton(SdlPlatform &platform,
                NovaFontCache &font_cache,
                const SDL_FRect &box,
                std::string_view label,
                bool highlighted) {
  SDL_Renderer *renderer = platform.renderer();
  SDL_SetRenderDrawColor(
      renderer, kPanel.r, kPanel.g, kPanel.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &box);
  const SDL_Color border = highlighted ? kTitle : kBorder;
  SDL_SetRenderDrawColor(
      renderer, border.r, border.g, border.b, SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &box);
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        12.0F,
                        kNovaFontStyleRegular,
                        highlighted ? kTitle : kBody,
                        box.x + 4.0F,
                        box.x + box.w - 4.0F,
                        box.y + box.h / 2.0F + 4.0F,
                        label);
}

// Fills `items` with the DITL item list sized to its window origin (a fresh
// SDL_FRect per item) and returns true when the dialog resources are usable.
struct SettingsLayout {
  SDL_FRect window{};
  std::vector<NovaDialogItem> items;
  bool from_ditl = false;
};

[[nodiscard]] SettingsLayout BuildSettingsLayout(const SDL_FRect &panel) {
  SettingsLayout out;
  const auto def = NovaResource_LoadDialogDefinition(kSettingsDialogId);
  const auto items = NovaResource_LoadDialogItems(kSettingsDialogId);
  if (!def || !items) {
    NovaLog::Todo("Settings DLOG/DITL 0x{:04x} unavailable; no dialog shown",
                  kSettingsDialogId);
    return out;
  }
  const float win_w = static_cast<float>(def->right - def->left);
  const float win_h = static_cast<float>(def->bottom - def->top);
  out.window = CenterWindowOnPanel(panel, win_w, win_h);
  out.items = *items;
  out.from_ditl = true;
  return out;
}

// One interactive control description resolved from the DITL for hit-testing.
struct SettingsControl {
  std::size_t index = 0;
  SDL_FRect rect{};
  bool checkbox = false;
  bool button = false;
  bool arrow = false;
  bool up = false;

  [[nodiscard]] bool is(const std::size_t i) const { return index == i; }
};

} // namespace

void KeyBindings::ResetToDefaults() {
  // Values exactly as NovaPrefs_ResetKeyBindings (0x004b4400) writes them.
  // Slot index == command id; value = bound key code (0xff unbound). See the
  // module comment and docs/preferences_keybindings.md for the flight row
  // (ASCII 'a' forward / 'c' turn-left / 'd' turn-right / 'f' reverse).
  cmd_to_key = {
      /* 0x00 */ 0x11, /*0x01*/ 0x1f, /*0x02*/ 0x39, /*0x03*/ 0x1d,
      /* 0x04 */ 0x15, /*0x05*/ 0x26, /*0x06*/ 0x1c, /*0x07*/ 0x1e,
      /* 0x08 */ 0x31, /*0x09*/ 0x32, /*0x0a*/ 0x29, /*0x0b*/ 0x13,
      /* 0x0c */ 0x23, /*0x0d*/ 0x2b, /*0x0e*/ 0x24, /*0x0f*/ 0x25,
      /* 0x10 */ 0x30, /*0x11*/ 0x2d, /*0x12*/ 0x0c, /*0x13*/ 0x63,
      /* 0x14 */ 0x64, /*0x15*/ 0x61, /*0x16*/ 0x66, /*0x17*/ 0x01,
      /* 0x18 */ 0x2c, /*0x19*/ 0x19, /*0x1a*/ 0x3b, /*0x1b*/ 0x3c,
      /* 0x1c */ 0x3d, /*0x1d*/ 0x3e, /*0x1e*/ 0x68, /*0x1f*/ 0x69,
      /* 0x20 */ 0x60, /*0x21*/ 0xff, /*0x22*/ 0xff, /*0x23*/ 0x40,
      /* 0x24 */ 0x05, /*0x25*/ 0x65, /*0x26*/ 0x62, /*0x27*/ 0x67,
      /* 0x28 */ 0x17, /*0x29*/ 0x16, /*0x2a*/ 0x12, /*0x2b*/ 0x02,
      /* 0x2c */ 0x03, /*0x2d*/ 0x04, /*0x2e*/ 0x05, /*0x2f*/ 0x06,
      /* 0x30 */ 0x21, /*0x31*/ 0x20, /*0x32*/ 0x2f, /*0x33*/ 0x2e,
      /* 0x34 */ 0x58,
  };
}

void NovaPreferences::ResetToDefaults() {
  bindings.ResetToDefaults();
  intro_music = true;
  sound_volume = 5;
  brightness = 3;
  share_processor_time = true;
  quicktime_movies = false;
  smoke_trails = false;
  run_in_window = false;
  ship_animations = true;
  engine_glows = true;
  running_lights = true;
  weapon_effects = true;
  parallax_starfield = true;
  ambient_sounds = true;
  hyperspace_effects = false;
  check_for_updates = true;
}

namespace {

// Whether a checkbox item should be drawn checked, honouring each toggle's
// inverted-ness (a box is checked when the feature is ON, which for the
// inverted flags means the global bit is 0). Uses the 0-based item index.
[[nodiscard]] bool CheckboxChecked(const NovaPreferences &prefs,
                                   std::size_t index) {
  auto on = [&](bool global) { return global; }; // normal toggle
  auto inverted = [&](bool global) { return global == false; };
  switch (index) {
  case 1:
    return on(prefs.share_processor_time);
  case 7:
    return on(prefs.intro_music);
  case 8:
    return inverted(prefs.quicktime_movies);
  case 9:
    return inverted(prefs.smoke_trails);
  case 10:
    return on(prefs.run_in_window);
  case 11:
    return on(prefs.ship_animations);
  case 12:
    return on(prefs.engine_glows);
  case 13:
    return on(prefs.running_lights);
  case 14:
    return on(prefs.weapon_effects);
  case 17:
    return on(prefs.parallax_starfield);
  case 19:
    return on(prefs.ambient_sounds);
  case 20:
    return inverted(prefs.hyperspace_effects);
  case 21:
    return inverted(prefs.check_for_updates);
  default:
    return false;
  }
}

// Flips the global bit for a toggled checkbox item, matching the original's
// `global = global == '\0'` writes.
void TogglePref(NovaPreferences &prefs, std::size_t index) {
  auto flip = [](bool &b) { b = !b; };
  switch (index) {
  case 1:
    flip(prefs.share_processor_time);
    break;
  case 7:
    flip(prefs.intro_music);
    break;
  case 8:
    flip(prefs.quicktime_movies);
    break;
  case 9:
    flip(prefs.smoke_trails);
    break;
  case 10:
    flip(prefs.run_in_window);
    break;
  case 11:
    flip(prefs.ship_animations);
    break;
  case 12:
    flip(prefs.engine_glows);
    break;
  case 13:
    flip(prefs.running_lights);
    break;
  case 14:
    flip(prefs.weapon_effects);
    break;
  case 17:
    flip(prefs.parallax_starfield);
    break;
  case 19:
    flip(prefs.ambient_sounds);
    break;
  case 20:
    flip(prefs.hyperspace_effects);
    break;
  case 21:
    flip(prefs.check_for_updates);
    break;
  default:
    break;
  }
}

// Hits one of the interactive DITL rects under the cursor. Returns the item
// index (0-based) or nullopt when no control is there.
[[nodiscard]] std::optional<std::size_t>
HitTestControl(const SettingsLayout &layout, float x, float y) {
  for (const auto &item : layout.items) {
    switch (item.type) {
    case 4:    // push button
    case 5:    // checkbox
    case 0x40: // slider arrow
      if (Contains(ItemRect(item, layout.window), x, y)) {
        return item.index;
      }
      break;
    default:
      break;
    }
  }
  return std::nullopt;
}

// Draws one frame of the Settings dialog over the dimmed playfield.
void DrawSettingsDialog(SdlPlatform &platform,
                        NovaFontCache &font_cache,
                        const SettingsLayout &layout,
                        const NovaPreferences &prefs,
                        std::optional<std::size_t> hover) {
  SDL_Renderer *renderer = platform.renderer();
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  platform.SetCenteredPlayfield();
  if (!layout.from_ditl) {
    return;
  }

  const auto &win = layout.window;
  // Dim the title screen behind the modal so it reads as a popped window.
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  const SDL_FRect full{0.0F, 0.0F, 640.0F, 480.0F};
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 130);
  SDL_RenderFillRect(renderer, &full);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  // Window panel + border (clean-room chrome; the DLOG has no backdrop PICT).
  SDL_SetRenderDrawColor(
      renderer, kPanel.r, kPanel.g, kPanel.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &win);
  SDL_SetRenderDrawColor(
      renderer, kBorder.r, kBorder.g, kBorder.b, SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &win);

  // Title band header (item 18), centred Chicago text.
  const NovaDialogItem *title = nullptr;
  for (const auto &item : layout.items) {
    if (item.index == 18) {
      title = &item;
      break;
    }
  }
  if (title != nullptr) {
    NovaText_DrawCentered(platform,
                          font_cache,
                          NovaFontFamily::kChicago,
                          18.0F,
                          kNovaFontStyleRegular,
                          kTitle,
                          win.x + static_cast<float>(title->left),
                          win.x + static_cast<float>(title->right),
                          win.y + static_cast<float>(title->top) + 16.0F,
                          "Preferences");
  }

  for (const auto &item : layout.items) {
    const SDL_FRect rect = ItemRect(item, win);
    switch (item.type) {
    case 5: // checkbox
      DrawCheckBox(platform,
                   font_cache,
                   rect,
                   item.title,
                   CheckboxChecked(prefs, item.index));
      break;
    case 4: // push button (OK / Key Settings)
      DrawButton(platform,
                 font_cache,
                 rect,
                 item.title,
                 hover && *hover == item.index);
      break;
    case 0x40: // slider arrow (upper cell = up, lower = down)
      DrawSliderArrow(
          platform, font_cache, rect, item.index == 6 || item.index == 25);
      break;
    case 0x08: // static value/label text
      if (item.index == 4) {
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      12.0F,
                      kNovaFontStyleRegular,
                      kBody,
                      rect.x + 4.0F,
                      rect.y + rect.h / 2.0F + 4.0F,
                      LoadSoundVolumeWord(prefs.sound_volume));
      } else if (item.index == 23) {
        // Brightness word source (STR# 0x8b) is not present in this archive;
        // show the numeric level instead (TODO(decomp): locate the word table).
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      12.0F,
                      kNovaFontStyleRegular,
                      kBody,
                      rect.x + 4.0F,
                      rect.y + rect.h / 2.0F + 4.0F,
                      std::to_string(prefs.brightness));
      } else {
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      12.0F,
                      kNovaFontStyleRegular,
                      kDim,
                      rect.x + 4.0F,
                      rect.y + rect.h / 2.0F + 4.0F,
                      item.title);
      }
      break;
    default:
      break;
    }
  }
}

} // namespace

// Ghidra: 0x00488650 Menu_RunSettingsDialog
bool NovaMenu_RunSettingsDialog(SdlPlatform &platform,
                                SdlAudio &audio,
                                SdlMusic &music,
                                NovaFontCache &font_cache,
                                NovaPreferences &prefs) {
  (void)audio;
  const SDL_FRect panel{0.0F, 0.0F, 640.0F, 480.0F};
  SettingsLayout layout = BuildSettingsLayout(panel);
  if (!layout.from_ditl) {
    NovaLog::Todo("cancelled: Settings dialog resources 0x{:04x} missing",
                  kSettingsDialogId);
    return false;
  }
  NovaLog::Info("opening Settings dialog (DLOG 0x{:04x})", kSettingsDialogId);

  while (!platform.quit_requested()) {
    const SDL_FPoint mouse = platform.mouse_position();
    const auto hover = HitTestControl(layout, mouse.x, mouse.y);
    DrawSettingsDialog(platform, font_cache, layout, prefs, hover);
    SDL_RenderPresent(platform.renderer());

    for (auto in = platform.PollTextEvent(); in;
         in = platform.PollTextEvent()) {
      switch (in->key) {
      case TextKey::escape:
        return false; // Cancel (discard changes).
      case TextKey::enter:
        return true; // Enter acts as OK.
      case TextKey::primary: {
        const auto hit = HitTestControl(layout, mouse.x, mouse.y);
        if (!hit) {
          break;
        }
        switch (*hit) {
        case 0: // OK
          return true;
        case 15: // Key Settings
          NovaLog::Todo(
              "Key Settings dialog (DLOG 0xfa2) is not reconstructed; "
              "returned unchanged");
          break;
        case 5: // sound down
          prefs.sound_volume =
              std::max<std::int32_t>(0, prefs.sound_volume - 1);
          break;
        case 6: // sound up
          prefs.sound_volume =
              std::min<std::int32_t>(8, prefs.sound_volume + 1);
          break;
        case 24: // brightness down
          prefs.brightness = std::max<std::int32_t>(0, prefs.brightness - 1);
          break;
        case 25: // brightness up
          prefs.brightness = std::min<std::int32_t>(6, prefs.brightness + 1);
          break;
        default:
          TogglePref(prefs, *hit);
          if (*hit == 10) {
            NovaLog::Warn("Run in a window toggle stored but not applied until "
                          "SdlPlatform gains a windowed-mode switch");
          }
          if (*hit == 7 && !prefs.intro_music && music.IsPlaying()) {
            music.Stop();
          } else if (*hit == 7 && prefs.intro_music) {
            music.Play();
          }
          break;
        }
        break;
      }
      default:
        break;
      }
    }
    SDL_Delay(16);
  }
  return false;
}

} // namespace game
