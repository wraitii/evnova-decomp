#pragma once

// Per-frame in-flight HUD overlay, reconstructed from the original's gameplay
// panel system. The government-specific cockpit PICT is a native-size 194x767
// strip anchored to the active render surface's top-right edge. Every decoded
// .ntf panel rect is translated by that same horizontal origin. The life bars
// are horizontal slots within the strip (e.g. Federation shield x 35..184,
// y 200..207) and fill from left to right -- see HudBar_FillRect.
//
// Ghidra model:
//   * Ui_InstallGameplayInterfaceLayout (0x004cda50) resolves the government
//     interface id from the player ship class's inherited government, loads
//     the `.ntf` interface-layout resource (key 0x6a918b9a) and fills the
//     g_*_panel_* rect/colour globals plus the cockpit PICT id (payload +0xa4,
//     clamped >= 0x80). That layout is decoded by
//     NovaResource_LoadGameplayInterfaceLayout (gameplay_interface.cpp).
//   * HUD panel paths translate layout rects by RenderOwner.right - 0xc2
//     (DAT_0088c020), anchoring the 194px cockpit strip to the top-right.
//   * NovaUi_DrawPlayerShieldArmorPanels (0x0045e9c0) / _FuelPanel (0x0045efe0)
//     draw the vertical life-support bars: the cockpit PICT blit already draws
//     the bar trough, then a sub-rect of the panel is filled with the gov's bar
//     colour to a fraction = current/max (NovaUi_DrawPlayerShieldBar
//     0x0045ea66).
//   * NovaUi_SetupGameplayPanelColors (0x0045cfc0) points the value/label/bar
//     colour pointers at the layout globals; the bar colours come from the
//     .ntf colour slots (+0x20 shield, +0x2c armor, +0x38 fuel, +0x3c fuel
//     reserve).
//
// The life bars and readouts are drawn inside their real, top-right-strip panel
// rects.

#include "game_state.hpp"
#include "gameplay_interface.hpp"
#include "nova_font.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <random>

class SdlPlatform;
class SdlTexture;
struct SDL_Renderer;

namespace game {

class SpriteStore;

class HudRenderer {
public:
  HudRenderer() = default;
  ~HudRenderer();
  HudRenderer(const HudRenderer &) = delete;
  HudRenderer &operator=(const HudRenderer &) = delete;

  // Resolves the player HUD interface id exactly as
  // Ui_InstallGameplayInterfaceLayout (player ship class -> inherited
  // government -> Government.interface_id, >= 0x80) and loads/decode+uploads
  // the matching interface layout + cockpit PICT. Called on spaceflight entry
  // and whenever the interface could change. Returns true when a usable layout
  // was installed (a missing gov falls back to the "Default" interface 0x80).
  // Never fails visibly; on total resource absence it installs no cockpit art
  // and the overlay falls back to flat bars.
  bool Install(SdlPlatform &platform, const GameState &state);

  // Composites the HUD overlay over the already-drawn free-flight world. The
  // cockpit PICT is drawn at native size pinned to the viewport top-right;
  // bars and readouts inherit that horizontal offset.
  void Draw(SdlPlatform &platform, const GameState &state);

  // Non-owning pointer to the spaceflight view's sprite store, used to
  // resolve each stellar body's spin sprite full height for the radar blip
  // size tiers (Sprite_GetFrameFullHeight 0x00462390 on the loaded spin set).
  void AttachSpriteStore(const SpriteStore *store) { sprite_store_ = store; }

  [[nodiscard]] bool installed() const { return installed_; }

private:
  // The government interface id whose layout is currently installed (the .ntf
  // resource id, >= 0x80). -1 when nothing usable was found.
  std::int16_t interface_id_ = -1;
  // The static layout (panel rects, colours, cockpit PICT id) as decoded.
  GameplayInterfaceLayout layout_{};
  // Whether the cockpit PICT backed this layout (always true on a successful
  // .ntf decode; used to fall back to flat-frame bars when it cannot load).
  bool installed_ = false;
  // The uploaded cockpit PICT texture (the gov interface background art, e.g.
  // PICT 0x2be for the Federation), or null when the resource cannot be decoded
  // (the life bars still render alone).
  std::unique_ptr<SdlTexture> cockpit_;
  int cockpit_w_ = 0;
  int cockpit_h_ = 0;
  // The screen-font cache used for the HUD readouts, kept for this renderer's
  // lifetime so font handles are decoded once (not reloaded every frame).
  std::unique_ptr<NovaFontCache> font_cache_;

  // Ship-class portraits for the target panel (PICT 3000 + zero-based clone
  // source class id, per the Bible "PICT resource ID 3000 + shipID - 128",
  // reused across classes that share base sprites via
  // ShipClassDef.clone_source_ship_class). Keyed by the zero-based ship class
  // id so each distinct class is decoded/uploaded once. A missing/failed
  // decode maps to null (the target panel draws text-only).
  struct PortraitEntry {
    std::unique_ptr<SdlTexture> texture;
    int width = 0;
    int height = 0;
  };

  std::map<std::int16_t, std::unique_ptr<PortraitEntry>> portraits_;

  // --- Stellar radar state (NovaUi_DrawStellarRadarPanel 0x0045d600) -----
  const SpriteStore *sprite_store_ = nullptr;
  // Ghidra g_last_target_status_poll_tick / g_target_status_blink_phase
  // (0x007cab74 / 0x007cab6c): the >= 15-tick target-status poll cadence that
  // toggles the blink phase inside NovaUi_RefreshGameplayPanels (0x0045d320).
  std::uint32_t radar_poll_ms_ = 0;
  std::int16_t radar_blink_phase_ = 0;
  // Sensor-static tiles drawn while a proximity scan is detected: the
  // original's 10 preloaded 'ppat' resources 128..137 (DAT_00733b7c), decoded
  // by Resource_LoadPixPatAsImage. On each >=15-tick radar refresh the original
  // picks one from the shared NovaRandom stream and tiles it
  // (DrawContext_TileImageInRect 0x004bbdc0); the port keeps its own stream
  // (divergence).
  // Divergence: the original searches the archive list newest-first
  // (FUN_004ff900 prepends on open), so ppat 128 resolves to Nova Graphics 1's
  // 8-bit pattern; the port's NovaResource_Load scans first-match, so ppat 128
  // resolves to Nova.rez's 4-bit grayscale pattern. ppat 128 is the only
  // duplicated resource key in the shipped archives, so this is confined to
  // radar static.
  bool radar_static_loaded_ = false;
  std::mt19937 radar_rng_{1337};
  std::array<std::unique_ptr<SdlTexture>, 10> radar_static_{};
  // Index of the pattern currently tiled. The original re-rolls it inside
  // NovaUi_DrawStellarRadarPanel, which only runs on the >= 15-tick radar
  // refresh; the port re-rolls it on that same poll and holds it between
  // frames because it composites directly every frame.
  std::size_t radar_static_index_ = 0;
  // Whether the interference static was selected at the last >= 15-tick radar
  // refresh. The original decides the static/contacts branch inside
  // NovaUi_DrawStellarRadarPanel and holds the result in the offscreen radar
  // buffer for the whole poll interval; because g_proximity_scan_detected is
  // re-rolled every simulation tick (Frame_RollProximityScanDetection
  // 0x0045d030, TickSystems scope 0xc) but only sampled at the draw, the port
  // must latch it on the poll instead of reading it per rendered frame.
  bool radar_static_active_ = false;

  // Loads (and caches) the target-panel portrait for a zero-based ship class
  // id, resolving the portrait PICT through the class's clone source, or null
  // when unavailable (null is also returned for a cached-but-textureless
  // entry, so callers may dereference the returned entry's texture freely).
  [[nodiscard]] const PortraitEntry *
  TargetPortrait(SdlPlatform &platform,
                 const ScenarioData &scenario,
                 std::int16_t class_id);

  // Ghidra 0x0045d600 NovaUi_DrawStellarRadarPanel. Draws the stellar radar
  // panel: cockpit backing (or IFF black fill), the current system's stellar
  // bodies and local ships as colour/size-tiered blips, the blinking primary
  // target blip, the far-from-origin direction arrow, and sensor static while
  // a proximity scan is detected.
  void DrawEscortCommandsPanel(SdlPlatform &platform,
                               const GameState &state,
                               const SDL_Color &value_color,
                               const SDL_Color &label_color);
  void DrawRadarPanel(SdlPlatform &platform, const GameState &state);

  // The four gameplay text panels, one per original draw path. Each mirrors
  // its Ghidra function: shared top-right anchor transform, label/value
  // colour split, STR# 0x7d2 strings and per-row cursor geometry.
  void DrawTravelPanel(SdlPlatform &platform,
                       const GameState &state,
                       const SDL_Color &value_color,
                       const SDL_Color &label_color);
  void DrawWeaponPanel(SdlPlatform &platform,
                       const GameState &state,
                       const SDL_Color &value_color,
                       const SDL_Color &label_color);
  void DrawTargetPanel(SdlPlatform &platform,
                       const GameState &state,
                       const SDL_Color &value_color,
                       const SDL_Color &label_color);
  void DrawCargoPanel(SdlPlatform &platform,
                      const GameState &state,
                      const SDL_Color &value_color,
                      const SDL_Color &label_color);
};

} // namespace game
