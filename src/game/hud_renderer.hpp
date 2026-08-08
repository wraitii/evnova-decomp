#pragma once

// Per-frame in-flight HUD overlay, reconstructed from the original's gameplay
// panel system. The game renders the HUD (the government-specific cockpit
// PICT plus the shield/armor/fuel life bars and the travel / weapon / target /
// cargo text readouts) on its fixed 1024x768 gameplay surface, which the
// 640x480 window displays at 0.625 scale. We reproduce that: the cockpit PICT
// and every panel rect are laid out in 1024-canvas coordinates (the genuine
// .ntf panel rects decoded by GameplayGeometry_FromSurface / the layout), and
// projected by kHudScale (640/1024) top-left anchored onto the window. Per the
// project's resolution policy higher in-game resolution shows more system (the
// extending viewport widens); the HUD chrome itself stays at this fixed 0.625
// scale. The life-support slots are thin vertical bars (e.g. Federation shield
// 200..207 x 35..184) filled from the top down -- see HudBar_FillRect.
//
// Ghidra model:
//   * Ui_InstallGameplayInterfaceLayout (0x004cda50) resolves the government
//     interface id from the player ship class's inherited government, loads
//     the `.ntf` interface-layout resource (key 0x6a918b9a) and fills the
//     g_*_panel_* rect/colour globals plus the cockpit PICT id (payload +0xa4,
//     clamped >= 0x80). That layout is decoded by
//     NovaResource_LoadGameplayInterfaceLayout (gameplay_interface.cpp).
//   * NovaView_UpdateGameplayViewport (0x00488380) bakes the 1024x768 frame
//     PICT 8000 into the gameplay surface and derives g_hud_panel_origin /
//     g_hud_panel_anchor from the surface rect (the anchor is (0,0) for a full
//     1024x768 canvas; large viewports nudge it via
//     GameplayGeometry_FromSurface).
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
// The life bars are drawn inside their real layout panel rects (the thin tall
// shield/armor/fuel slots near the top-left of the canvas), and the readout
// panels (travel / weapon ammo / target / cargo) at their genuine positions
// across the top band.

#include "game_state.hpp"
#include "gameplay_interface.hpp"
#include "nova_font.hpp"

#include <cstdint>
#include <memory>

class SdlPlatform;
class SdlTexture;
struct SDL_Renderer;

namespace game {

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
  // cockpit PICT is drawn at native size pinned to the viewport top-left (the
  // HUD is never scaled). Bars and readouts are drawn in the interface's panel
  // rects, offset by the derived HUD anchor for the current viewport size.
  void Draw(SdlPlatform &platform, const GameState &state);

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
};

} // namespace game
