#include "spaceflight_view.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../rle_sprite_sheet.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "ship_visual.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace game {
namespace {

constexpr float kTwoPi = 6.283185307179586F;
// Logical play area: the space viewport occupies the 640x400 region above the
// HUD strip (the existing placeholder HUD sits at y 400..460).
constexpr int kViewportWidth = 640;
constexpr int kViewportHeight = 400;

// Frame index for a heading. EV Nova ships point 'up' at frame 0 with heading
// increasing clockwise; frames progress one per sector of the rotation.
// TODO(decomp): verify phase/clockwise orientation against a real rendered
// ship -- this is the conventional mapping and should be re-checked once the
// ship is on screen.
[[nodiscard]] int FrameForHeading(float heading_radians, int frames_per_rotation) {
  const float normalized = std::fmod(heading_radians + kTwoPi, kTwoPi);
  const float sector =
      (normalized / kTwoPi) * static_cast<float>(frames_per_rotation);
  int frame = static_cast<int>(std::lround(sector)) % frames_per_rotation;
  if (frame < 0) {
    frame += frames_per_rotation;
  }
  return frame;
}

} // namespace

bool SpaceflightView::EnsureShipSprite(SdlPlatform &platform,
                                       const GameState &state) {
  if (!ship_.frames.empty()) {
    return true;
  }
  const auto *ship_class = state.scenario.Ship(
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (!ship_class) {
    NovaLog::Warn("ship sprite: no ship class {:#x} in scenario tables",
                  static_cast<unsigned>(state.player.ship_class_id + 0x80));
    return false;
  }
  // The sh\x8an descriptor id matches the ship class id.
  const auto visual_data = NovaResource_Load(
      kShipVisualResourceType,
      static_cast<std::uint16_t>(state.player.ship_class_id + 0x80));
  if (!visual_data) {
    NovaLog::Warn("ship sprite: no sh.x9an descriptor for class '{}'",
                  ship_class->display_name);
    return false;
  }
  const auto visual = DecodeShipVisualDescriptor(*visual_data);
  if (!visual) {
    NovaLog::Warn("ship sprite: malformed sh.x9an descriptor for class '{}'",
                  ship_class->display_name);
    return false;
  }
  const auto sheet_data = NovaResource_Load(kResourceTypeRleSheet16,
                                            visual->base_image_id);
  if (!sheet_data) {
    NovaLog::Warn("ship sprite: no rl.x91D sheet {} for class '{}'",
                  visual->base_image_id, ship_class->display_name);
    return false;
  }
  auto sheet = RleSpriteSheet_Decode16(*sheet_data);
  if (!sheet) {
    NovaLog::Warn("ship sprite: failed to decode rl.x91D sheet {} for class '{}'",
                  visual->base_image_id, ship_class->display_name);
    return false;
  }
  ship_.frames_per_rotation = visual->frames_per_rotation;
  ship_.frame_count = static_cast<int>(sheet->frames.size());
  ship_.width = sheet->width;
  ship_.height = sheet->height;
  ship_.frames.reserve(sheet->frames.size());
  SDL_Renderer *const renderer = platform.renderer();
  for (const auto &frame : sheet->frames) {
    auto texture = SdlTexture::Create(renderer, sheet->width, sheet->height,
                                      frame.rgba_pixels);
    if (!texture) {
      NovaLog::Warn("ship sprite: texture upload failed for class '{}'",
                    ship_class->display_name);
      ship_.frames.clear();
      return false;
    }
    ship_.frames.push_back(std::move(texture));
  }
  NovaLog::Info("ship sprite loaded for '{}': {}x{} x{} frames ({} set(s))",
                ship_class->display_name, sheet->width, sheet->height,
                ship_.frame_count, visual->base_set_count);
  return true;
}

// Loads (and caches) the spin sprite set for a stellar's graphic. spin_set_id
// is the stellar's link_a_id; the sp\x9an descriptor id is spin_set_id + 1000
// (the stellar-object spin id range), and its SpritesID names the rl\x91D sheet.
const SpaceflightView::SpinSpriteSet *
SpaceflightView::GetSpinSpriteSet(SdlPlatform &platform, int spin_set_id) {
  if (spin_set_id < 0 || spin_set_id > 0xff) {
    return nullptr;
  }
  const auto index = static_cast<std::size_t>(spin_set_id);
  if (spin_sets_.size() <= index) {
    spin_sets_.resize(index + 1);
  }
  if (spin_sets_[index]) {
    return spin_sets_[index].get();  // cached (possibly a failed load -> null)
  }
  auto owner = std::make_unique<SpinSpriteSet>();
  const auto spin_id = static_cast<std::uint16_t>(spin_set_id + 1000);
  const auto spin_data = NovaResource_Load(kResourceTypeSprites, spin_id);
  if (spin_data) {
    const auto def = NovaSpriteDefinition_Parse(*spin_data);
    if (def) {
      const auto sheet_data =
          NovaResource_Load(kResourceTypeRleSheet16, def->sprites_resource_id);
      if (sheet_data) {
        auto sheet = RleSpriteSheet_Decode16(*sheet_data);
        if (sheet && def->tile_width > 0 && def->tile_height > 0 &&
            sheet->width == def->tile_width &&
            sheet->height == def->tile_height) {
          owner->frame_count = static_cast<int>(sheet->frames.size());
          owner->width = def->tiles_x;
          owner->height = def->tiles_y;
          owner->tile_width = def->tile_width;
          owner->tile_height = def->tile_height;
          for (const auto &frame : sheet->frames) {
            auto texture = SdlTexture::Create(
                platform.renderer(), sheet->width, sheet->height,
                frame.rgba_pixels);
            if (!texture) {
              break;
            }
            owner->frames.push_back(std::move(texture));
          }
          NovaLog::Info("spin sprite id {}: {}x{} {} frames", spin_id,
                        sheet->width, sheet->height, owner->frame_count);
        }
      }
    }
  }
  if (owner->frames.empty()) {
    NovaLog::Warn("no spin sprite for stellar set {} (spin id {})",
                  spin_set_id, spin_id);
    return nullptr;  // not cached; caller falls back to tinted disc
  }
  spin_sets_[index] = std::move(owner);
  return spin_sets_[index].get();
}

// Draws the parallax starfield for the current system: two slow layers of
// deterministic star "clouds" scrolled by (player_pos * parallax) so drifting
// conveys motion, plus a faint background tint derived from the system's
// government theme color when a system is active.
void DrawStarfield(SDL_Renderer *renderer, const GameState &state) {
  // Background wash tinted by the current system's government theme colour (so
  // systems read distinctly), lightened so stars still show.
  std::uint8_t bg_r = 6, bg_g = 8, bg_b = 24;
  if (const auto *sys =
          state.scenario.System(static_cast<std::int16_t>(state.player.current_system_id + 0x80));
      sys) {
    if (const auto *gov = state.scenario.Government(sys->government_id); gov) {
      bg_r = static_cast<std::uint8_t>((gov->theme_red >> 1U) + 8U);
      bg_g = static_cast<std::uint8_t>((gov->theme_green >> 1U) + 8U);
      bg_b = static_cast<std::uint8_t>((gov->theme_blue >> 1U) + 14U);
    }
  }
  SDL_SetRenderDrawColor(renderer, bg_r, bg_g, bg_b, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);

  // Two parallax star layers. The exact positions are deterministic but spread
  // evenly; scrolling by player position gives a subtle depth feel.
  const struct {
    int count;
    int parity;
    float parallax;
    std::uint8_t r, g, b;
  } layers[] = {
      {110, 3, 0.25F, 150, 195, 255},  // distant sparse faint stars
      {45, 7, 0.55F, 210, 235, 255},   // nearer brighter stars
  };
  SDL_SetRenderDrawColor(renderer, 255, 255, 255, SDL_ALPHA_OPAQUE);
  for (const auto &layer : layers) {
    SDL_SetRenderDrawColor(renderer, layer.r, layer.g, layer.b,
                           SDL_ALPHA_OPAQUE);
    for (int i = 0; i < layer.count; ++i) {
      const float freq_x = 1.0F + static_cast<float>(i % 7) * 0.113F;
      const float freq_y = 1.0F + static_cast<float>((i * 5) % 11) * 0.071F;
      const float base_x =
          20.0F + 600.0F * std::fmod(static_cast<float>(i) * 0.6180339F + 0.17F, 1.0F);
      const float base_y =
          16.0F + 384.0F * std::fmod(static_cast<float>(i * 0.381966F) + 0.53F, 1.0F);
      float sx = std::fmod(base_x - state.player.pos_x * layer.parallax * freq_x,
                           static_cast<float>(kViewportWidth));
      float sy = std::fmod(base_y - state.player.pos_y * layer.parallax * freq_y,
                           static_cast<float>(kViewportHeight));
      if (sx < 0.0F) sx += static_cast<float>(kViewportWidth);
      if (sy < 0.0F) sy += static_cast<float>(kViewportHeight);
      SDL_RenderPoint(renderer, sx, sy);
    }
  }
}

// Draws the current system's stellar bodies. When the body's spin sprite set
// (link_a_id -> sp\x9an id+1000) loads, its real planet art is drawn; otherwise
// a tinted circle stands in. Positions are world px offset by the player
// (camera centred on the ship).
void SpaceflightView::DrawStellarBodies(SdlPlatform &platform,
                                        const GameState &state) {
  SDL_Renderer *const renderer = platform.renderer();
  const auto *sys =
      state.scenario.System(static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (!sys) {
    return;
  }
  for (std::size_t slot = 0; slot <= 0x100; ++slot) {
    const auto *st =
        state.scenario.Stellar(static_cast<std::int16_t>(slot + 0x80));
    if (!st || st->name.empty()) {
      continue;
    }
    // Skip the space ports / stations that the scenario leaves at (0,0) stacked
    // on top of the landing zone: they would all pile at the camera centre.
    if (st->pos_x == 0 && st->pos_y == 0) {
      continue;
    }
    const int cx =
        (st->pos_x - static_cast<int>(state.player.pos_x)) + kViewportWidth / 2;
    const int cy =
        (st->pos_y - static_cast<int>(state.player.pos_y)) + kViewportHeight / 2;
    if (cx < -160 || cx > kViewportWidth + 160 || cy < -160 ||
        cy > kViewportHeight + 160) {
      continue;  // off-screen
    }
    // Pick an 8-bit tint: the stellar's government (now decoded) if present,
    // else the system government.
    std::uint8_t r = 170, g = 170, b = 200;
    const auto *gov =
        st->government_id >= 0x80 ? state.scenario.Government(st->government_id)
                                  : state.scenario.Government(sys->government_id);
    if (gov && gov->present) {
      r = gov->theme_red;
      g = gov->theme_green;
      b = gov->theme_blue;
    }

    // Prefer the real spin planet sprite; fall back to a tinted disc.
    const auto *set = GetSpinSpriteSet(platform, st->link_a_id);
    if (set && !set->frames.empty()) {
      const auto &frame = set->frames[0];
      const float scale = 1.0F;  // planets render at native world size
      const SDL_FRect dest{
          static_cast<float>(cx) - set->tile_width * scale / 2.0F,
          static_cast<float>(cy) - set->tile_height * scale / 2.0F,
          set->tile_width * scale, set->tile_height * scale};
      SDL_RenderTexture(renderer, frame->get(), nullptr, &dest);
      continue;
    }
    const int radius = 14;
    const SDL_FRect rect{static_cast<float>(cx - radius),
                         static_cast<float>(cy - radius),
                         static_cast<float>(radius * 2),
                         static_cast<float>(radius * 2)};
    SDL_SetRenderDrawColor(renderer, r / 2U, g / 2U, b / 2U, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, r, g, b, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &rect);
  }
}

void SpaceflightView::Draw(SdlPlatform &platform, const GameState &state) {
  SDL_Renderer *const renderer = platform.renderer();
  DrawStarfield(renderer, state);
  DrawStellarBodies(platform, state);

  // Player ship at the play-area centre, frame selected by heading.
  if (!ship_.frames.empty() && ship_.frames_per_rotation > 0) {
    const int frame = FrameForHeading(state.player.heading,
                                      ship_.frames_per_rotation);
    const int clamped_frame = std::clamp(frame, 0, ship_.frame_count - 1);
    const auto &texture = ship_.frames[static_cast<std::size_t>(clamped_frame)];
    const float scale = 1.0F;  // original draws ship at native size
    const float w = static_cast<float>(ship_.width) * scale;
    const float h = static_cast<float>(ship_.height) * scale;
    const SDL_FRect dest{
        static_cast<float>(kViewportWidth) / 2.0F - w / 2.0F,
        static_cast<float>(kViewportHeight) / 2.0F - h / 2.0F, w, h};
    SDL_RenderTexture(renderer, texture->get(), nullptr, &dest);
  }
}

} // namespace game
