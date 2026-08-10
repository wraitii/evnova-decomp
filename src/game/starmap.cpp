#include "starmap.hpp"

#include "../log.hpp"
#include "nova_font.hpp"
#include "scenario_data.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace game {
namespace {

// ---- Fixed UI geometry (logical 640x480 playfield) ------------------------
// The map modal draws over a centred 640x480 logical field (the same space the
// other modals use). A title bar sits at the top; the galaxy graph fills the
// panel below it; an inspector footer shows the selected system's details.
constexpr int kPanelX = 8;
constexpr int kPanelY = 8;
constexpr int kPanelW = 624;
constexpr int kPanelH = 430;
constexpr float kTitleBaselineY = 22.0F;
constexpr float kFooterBaselineY = 452.0F;
constexpr float kHelpBaselineY = 470.0F;

// Galaxies can span large/unbounded coordinates; the map view transform maps
// the bounded set of *reachable* system positions (clamped to the current
// system's neighbourhood plus explored neighbours) into the panel.
constexpr float kMarkerRadius = 4.0F;
constexpr float kCurrentSystemRadius = 6.0F;

// Colours.
constexpr SDL_Color kWindowBg{0, 0, 0, 255};
constexpr SDL_Color kPanelBg{6, 14, 26, 255};
constexpr SDL_Color kScrim{0, 0, 0, 180};
constexpr SDL_Color kPanelBorder{72, 120, 168, 255};
constexpr SDL_Color kLinkLine{58, 82, 116, 200};
constexpr SDL_Color kLinkLineCurrent{96, 150, 214, 255};
constexpr SDL_Color kMarkerExplored{150, 196, 244, 255};
constexpr SDL_Color kMarkerUnexplored{70, 96, 128, 255};
constexpr SDL_Color kMarkerCurrent{255, 214, 100, 255};
constexpr SDL_Color kTextTitle{202, 224, 255, 255};
constexpr SDL_Color kTextBody{200, 214, 232, 255};
constexpr SDL_Color kTextDim{110, 132, 158, 255};
constexpr SDL_Color kSelectionBox{110, 170, 230, 255};

// A single system marker's mapped screen position plus its index, so clicks
// and label collision checks resolve against the same projection as drawing.
struct MappedSystem {
  std::int16_t zero_based_id = -1; // scenario.systems index
  float sx = 0.0F;                 // panel-local x
  float sy = 0.0F;                 // panel-local y
};

// The map view: an SRT-ish transform that places the galaxy bounding box into
// the panel with a user-adjustable scale. Pan/zoom are trivial keyboard ops on
// this structure; clicks map mouse -> world through the inverse.
struct MapView {
  float scale = 1.0F; // panel-px per world-unit
  float offset_x = 0.0F;
  float offset_y = 0.0F;

  [[nodiscard]] float ToScreenX(float world_x) const {
    return offset_x + world_x * scale;
  }
  [[nodiscard]] float ToScreenY(float world_y) const {
    return offset_y + world_y * scale;
  }
  [[nodiscard]] float ToWorldX(float screen_x) const {
    return (screen_x - offset_x) / scale;
  }
  [[nodiscard]] float ToWorldY(float screen_y) const {
    return (screen_y - offset_y) / scale;
  }
  void PanPixels(float dx, float dy) {
    offset_x += dx;
    offset_y += dy;
  }
  void ZoomAt(float factor, float screen_x, float screen_y) {
    // Keep the world point under the cursor stationary while scaling.
    const float world_x = ToWorldX(screen_x);
    const float world_y = ToWorldY(screen_y);
    scale = std::clamp(scale * factor, 0.25F, 8.0F);
    offset_x = screen_x - world_x * scale;
    offset_y = screen_y - world_y * scale;
  }
};

// Computes the world->panel transform that fits the given world bounding box
// into the panel. Returns false when there is no finite extent to fit (empty /
// degenerate galaxy), in which case the map is left unchanged.
bool FitMapView(std::span<const System> systems,
                MapView &out_view,
                float panel_w,
                float panel_h) {
  float min_x = 0.0F, min_y = 0.0F, max_x = 0.0F, max_y = 0.0F;
  bool seeded = false;
  for (const auto &sys : systems) {
    const float px = static_cast<float>(sys.pos_x);
    const float py = static_cast<float>(sys.pos_y);
    if (!seeded) {
      min_x = max_x = px;
      min_y = max_y = py;
      seeded = true;
    } else {
      min_x = std::min(min_x, px);
      max_x = std::max(max_x, px);
      min_y = std::min(min_y, py);
      max_y = std::max(max_y, py);
    }
  }
  if (!seeded) {
    return false;
  }
  const float span_x = std::max(1.0F, max_x - min_x);
  const float span_y = std::max(1.0F, max_y - min_y);
  const float margin = 24.0F;
  const float avail_w = std::max(1.0F, panel_w - 2.0F * margin);
  const float avail_h = std::max(1.0F, panel_h - 2.0F * margin);
  const float fit = std::min(avail_w / span_x, avail_h / span_y);
  out_view.scale = std::min(fit, 4.0F);
  // Centre the world bounding box in the panel.
  const float centre_x = (min_x + max_x) * 0.5F;
  const float centre_y = (min_y + max_y) * 0.5F;
  out_view.offset_x = panel_w * 0.5F - centre_x * out_view.scale;
  out_view.offset_y = panel_h * 0.5F - centre_y * out_view.scale;
  return true;
}

// Whether the pilot has explored `zero_based_id` (the GameState explored bit
// mirrored to the scenario's per-system flag). Interprets ids conservatively:
// an out-of-range id is not explored.
bool SystemExplored(const GameState &state, std::int16_t zero_based_id) {
  if (zero_based_id < 0) {
    return false;
  }
  const std::size_t idx = static_cast<std::size_t>(zero_based_id);
  if (idx < state.control.explored_systems.size()) {
    return state.control.explored_systems.test(idx);
  }
  return false;
}

// Draws the fixed chrome: the window background, panel frame and title bar.
void DrawChrome(SdlPlatform &platform, NovaFontCache &font_cache) {
  SDL_Renderer *renderer = platform.renderer();
  SDL_SetRenderDrawColor(
      renderer, kWindowBg.r, kWindowBg.g, kWindowBg.b, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  platform.SetCenteredPlayfield();

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  const SDL_FRect full{
      0, 0, static_cast<float>(kLogicalUiWidth), static_cast<float>(kLogicalUiHeight)};
  SDL_SetRenderDrawColor(renderer, kScrim.r, kScrim.g, kScrim.b, kScrim.a);
  SDL_RenderFillRect(renderer, &full);
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

  const SDL_FRect panel{kPanelX, kPanelY, kPanelW, kPanelH};
  SDL_SetRenderDrawColor(renderer, kPanelBg.r, kPanelBg.g, kPanelBg.b, SDL_ALPHA_OPAQUE);
  SDL_RenderFillRect(renderer, &panel);
  SDL_SetRenderDrawColor(renderer, kPanelBorder.r, kPanelBorder.g, kPanelBorder.b, SDL_ALPHA_OPAQUE);
  SDL_RenderRect(renderer, &panel);

  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kChicago,
                        16.0F,
                        kNovaFontStyleBold,
                        kTextTitle,
                        0.0F,
                        640.0F,
                        kTitleBaselineY,
                        "Galaxy Map");
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        11.0F,
                        kNovaFontStyleRegular,
                        kTextDim,
                        0.0F,
                        640.0F,
                        kHelpBaselineY,
                        "Arrows pan   +/- zoom   h home/fit   click select "
                        "  Esc close");
}

// Draws the galaxy graph: link lines first, then markers and names.
void DrawGalaxy(SdlPlatform &platform,
                NovaFontCache &font_cache,
                const GameState &state,
                const std::vector<MappedSystem> &mapped,
                const MapView &view,
                std::int16_t selected_id) {
  const auto &systems = state.scenario.systems;
  const std::int16_t current = state.player.current_system_id;
  SDL_Renderer *renderer = platform.renderer();

  // Link lines, avoiding duplicates (links are symmetric in the System table:
  // system A lists B and B lists A).
  const auto draw_link = [&](std::int16_t a, std::int16_t b) {
    if (a < 0 || b < 0) {
      return;
    }
    const std::size_t idx_a = static_cast<std::size_t>(a);
    const std::size_t idx_b = static_cast<std::size_t>(b);
    if (idx_a >= systems.size() || idx_b >= systems.size()) {
      return;
    }
    const System &sa = systems[idx_a];
    const System &sb = systems[idx_b];
    const SDL_FPoint pa{view.ToScreenX(static_cast<float>(sa.pos_x)),
                        view.ToScreenY(static_cast<float>(sa.pos_y))};
    const SDL_FPoint pb{view.ToScreenX(static_cast<float>(sb.pos_x)),
                        view.ToScreenY(static_cast<float>(sb.pos_y))};
    const bool touches_current = a == current || b == current;
    SDL_SetRenderDrawColor(renderer,
                           touches_current ? kLinkLineCurrent.r
                                           : kLinkLine.r,
                           touches_current ? kLinkLineCurrent.g : kLinkLine.g,
                           touches_current ? kLinkLineCurrent.b : kLinkLine.b,
                           touches_current ? kLinkLineCurrent.a : kLinkLine.a);
    SDL_RenderLine(renderer, pa.x, pa.y, pb.x, pb.y);
  };

  for (std::size_t i = 0; i < systems.size(); ++i) {
    const System &sys = systems[i];
    for (const std::int16_t link : sys.links) {
      if (link < 0) {
        continue;
      }
      const std::int16_t zero_based = static_cast<std::int16_t>(link - 0x80);
      if (zero_based >= 0 && static_cast<std::size_t>(zero_based) > i) {
        draw_link(static_cast<std::int16_t>(i), zero_based);
      }
    }
  }

  // Markers + names.
  for (const auto &m : mapped) {
    const bool is_current = m.zero_based_id == current;
    const bool explored = SystemExplored(state, m.zero_based_id);
    const bool selected = m.zero_based_id == selected_id;
    SDL_Color c = explored ? kMarkerExplored : kMarkerUnexplored;
    if (is_current) {
      c = kMarkerCurrent;
    }
    if (selected) {
      c = kSelectionBox;
    }
    const float r = is_current ? kCurrentSystemRadius : kMarkerRadius;
    // Round the marker as a filled circle via a small octagon-sampled fill.
    constexpr int kSegments = 12;
    std::array<SDL_FPoint, kSegments + 1> pts{};
    for (int s = 0; s <= kSegments; ++s) {
      const float ang =
          static_cast<float>(s) * static_cast<float>(2.0 * std::numbers::pi) /
          static_cast<float>(kSegments);
      pts[static_cast<std::size_t>(s)] = SDL_FPoint{
          m.sx + r * std::cos(ang), m.sy + r * std::sin(ang)};
    }
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, SDL_ALPHA_OPAQUE);
    SDL_RenderLines(renderer, pts.data(), kSegments + 1);
    // Selection ring for the highlighted system.
    if (selected) {
      constexpr int kRingSegments = 16;
      std::array<SDL_FPoint, kRingSegments + 1> ring{};
      for (int s = 0; s <= kRingSegments; ++s) {
        const float ang =
            static_cast<float>(s) *
            static_cast<float>(2.0 * std::numbers::pi) /
            static_cast<float>(kRingSegments);
        ring[static_cast<std::size_t>(s)] =
            SDL_FPoint{m.sx + (r + 3.0F) * std::cos(ang),
                       m.sy + (r + 3.0F) * std::sin(ang)};
      }
      SDL_SetRenderDrawColor(renderer,
                             kSelectionBox.r,
                             kSelectionBox.g,
                             kSelectionBox.b,
                             SDL_ALPHA_OPAQUE);
      SDL_RenderLines(renderer, ring.data(), kRingSegments + 1);
    }
    // Label the explored / current / selected systems only, to keep the map
    // readable (unexplored names are hidden until discovered, matching the
    // original's fog of war).
    if (is_current || explored || selected) {
      if (m.zero_based_id >= 0 &&
          static_cast<std::size_t>(m.zero_based_id) < systems.size()) {
        NovaText_Draw(platform,
                      font_cache,
                      NovaFontFamily::kGeneva,
                      10.0F,
                      is_current ? kNovaFontStyleBold : kNovaFontStyleRegular,
                      is_current ? kMarkerCurrent : kTextBody,
                      m.sx + 6.0F,
                      m.sy - 4.0F,
                      systems[static_cast<std::size_t>(m.zero_based_id)].name);
      }
    }
  }
}

// Draws the inspector footer for the selected system.
void DrawInspector(SdlPlatform &platform,
                   NovaFontCache &font_cache,
                   const GameState &state,
                   std::int16_t selected_id) {
  std::string line = "No system selected";
  if (selected_id >= 0 &&
      static_cast<std::size_t>(selected_id) < state.scenario.systems.size()) {
    const System &sys =
        state.scenario.systems[static_cast<std::size_t>(selected_id)];
    const bool explored = SystemExplored(state, selected_id);
    const bool is_current = selected_id == state.player.current_system_id;
    line = sys.name + "   ";
    line += is_current ? "[current]" : (explored ? "[explored]" : "[undiscovered]");
    // Count reachable jumps from this system.
    int jump_count = 0;
    for (const std::int16_t link : sys.links) {
      if (link >= 0x80) {
        ++jump_count;
      }
    }
    line += "  jumps: " + std::to_string(jump_count);
  }
  NovaText_DrawCentered(platform,
                        font_cache,
                        NovaFontFamily::kGeneva,
                        12.0F,
                        kNovaFontStyleRegular,
                        kTextBody,
                        0.0F,
                        640.0F,
                        kFooterBaselineY,
                        line);
}

// Builds the projected marker list for the current view.
std::vector<MappedSystem> BuildMappedSystems(const GameState &state,
                                             const MapView &view,
                                             const SDL_FRect &panel) {
  std::vector<MappedSystem> out;
  out.reserve(state.scenario.systems.size());
  for (std::size_t i = 0; i < state.scenario.systems.size(); ++i) {
    const System &sys = state.scenario.systems[i];
    MappedSystem m;
    m.zero_based_id = static_cast<std::int16_t>(i);
    m.sx = panel.x + view.ToScreenX(static_cast<float>(sys.pos_x));
    m.sy = panel.y + view.ToScreenY(static_cast<float>(sys.pos_y));
    out.push_back(m);
  }
  return out;
}

// Distance-squared from a panel point to a system marker, in panel px.
float MarkerDistSq(const SDL_FPoint &point, const MappedSystem &m) {
  const float dx = point.x - m.sx;
  const float dy = point.y - m.sy;
  return dx * dx + dy * dy;
}

} // namespace

// ---------------------------------------------------------------------------
// NovaStarmap_RunWindow (0x004a3aa0 NovaUi_RunStarmapWindow, clean-room).
// ---------------------------------------------------------------------------
StarmapExit NovaStarmap_RunWindow(SdlPlatform &platform, GameState &state) {
  // No scenario tables -> nothing to map.
  if (state.scenario.systems.empty()) {
    NovaLog::Warn("starmap opened with no scenario system table; closing");
    return StarmapExit::kContinue;
  }

  NovaFontCache font_cache;
  const SDL_FRect panel{
      static_cast<float>(kPanelX),
      static_cast<float>(kPanelY),
      static_cast<float>(kPanelW),
      static_cast<float>(kPanelH),
  };

  // Initial fit: the whole (clamped) galaxy bounds, or the current system's
  // explored neighbourhood when the galaxy overflows into a sensible zoom.
  MapView view;
  FitMapView({state.scenario.systems.data(), state.scenario.systems.size()},
             view,
             panel.w,
             panel.h);

  std::int16_t selected_id = state.player.current_system_id;

  NovaLog::Info("opening galaxy starmap ({} systems in scenario)",
                state.scenario.systems.size());

  while (!platform.quit_requested()) {
    const auto mapped = BuildMappedSystems(state, view, panel);
    DrawChrome(platform, font_cache);
    DrawGalaxy(platform, font_cache, state, mapped, view, selected_id);
    DrawInspector(platform, font_cache, state, selected_id);
    SDL_RenderPresent(platform.renderer());

    // Process one batch of raw editable keys / clicks.
    for (std::optional<TextInput> in; (in = platform.PollTextEvent());) {
      switch (in->key) {
      case TextKey::escape:
        return StarmapExit::kContinue;

      case TextKey::enter:
        // Selecting the current system's own node (or the current default) is
        // a no-op navigation-wise; Enter confirms the selection and closes.
        return StarmapExit::kContinue;

      case TextKey::character: {
        const char ch = in->character;
        if (ch == 'q' || ch == 'x') {
          return StarmapExit::kContinue;
        }
        if (ch == '+' || ch == '=') {
          view.ZoomAt(1.35F, panel.w * 0.5F, panel.h * 0.5F);
        } else if (ch == '-') {
          view.ZoomAt(1.0F / 1.35F, panel.w * 0.5F, panel.h * 0.5F);
        } else if (ch == 'h' || ch == 'H') {
          FitMapView({state.scenario.systems.data(),
                      state.scenario.systems.size()},
                     view,
                     panel.w,
                     panel.h);
        }
        break;
      }

      case TextKey::primary: {
        // Select the nearest marker within a click radius; empty click keeps
        // the current selection.
        const SDL_FPoint mp = platform.mouse_position();
        std::int16_t best = -1;
        float best_d = 18.0F * 18.0F;
        for (const auto &m : mapped) {
          const float d = MarkerDistSq(mp, m);
          if (d < best_d) {
            best_d = d;
            best = m.zero_based_id;
          }
        }
        if (best >= 0) {
          selected_id = best;
        }
        break;
      }

      case TextKey::backspace:
      case TextKey::none:
        break;
      }
    }

    // Non-blocking keyboard pan/zoom polled from the live state so holding a
    // key pans continuously (mirrors the original's interactive panning).
    const bool *const keys = SDL_GetKeyboardState(nullptr);
    constexpr float kPanPxPerEvent = 10.0F;
    if (keys[SDL_SCANCODE_LEFT]) {
      view.PanPixels(-kPanPxPerEvent, 0.0F);
    }
    if (keys[SDL_SCANCODE_RIGHT]) {
      view.PanPixels(kPanPxPerEvent, 0.0F);
    }
    if (keys[SDL_SCANCODE_UP]) {
      view.PanPixels(0.0F, -kPanPxPerEvent);
    }
    if (keys[SDL_SCANCODE_DOWN]) {
      view.PanPixels(0.0F, kPanPxPerEvent);
    }
  }

  return StarmapExit::kQuit;
}

} // namespace game
