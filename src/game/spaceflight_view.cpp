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
#include <random>
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

// Uniform integer in [0, bound). Mirrors the game's NovaRandom_Range seeded from
// the GameState PRNG so the spawn layout is reproducible per session.
[[nodiscard]] std::int16_t NovaRandomRange(std::mt19937 &rng, int bound) {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::int16_t>(std::uniform_int_distribution<int>{0, bound - 1}(rng));
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

// Ghidra NovaEffects_QueuedAmbientStarParticles (0x0046ebf0). See the header.
// Decoded from the binary: spawn count = round(viewportHeight / 600.0 * 20.0)
// (divisor g_background_star_spawn_height_divisor = 600.0). Each of the first
// `count` slots gets a random world offset within the (player-centred) viewport
// and a parallax speed of NovaRandom_Range(0x23) * 0.01 (constant _DAT_00575738,
// a double). The remaining (20-count) slots are merely re-activated, keeping
// their previous position/speed (a re-scatter only rewrites the first `count`).
// A negative system murk (SystemDef.murk) clears the whole field. When the
// per-gameplay options toggle DAT_005914d7 is clear (starfield motion disabled)
// the speed is forced to zero so the field is static.
void SpaceflightView::SpawnAmbientStars(GameState &state) {
  const auto *sys = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  const bool murk_hides_stars = sys && sys->murk < 0;

  if (murk_hides_stars) {
    // Ghidra NovaEffects_ClearAmbientStarParticles: murk < 0 hides the stars.
    for (auto &s : ambient_stars_) {
      s = {};
    }
    return;
  }

  // Ghidra: count = round(viewportHeight / 600.0 * 20.0), clamped to the
  // 20-slot pool (viewport height 400 -> round(13.33) = 13 fresh stars).
  const int count =
      std::clamp<int>(static_cast<int>(std::round(kViewportHeight / 600.0F * 20.0F)), 0, 20);
  // gh.data flag DAT_005914d7: options toggle for starfield motion. Our clean
  // reimplementation currently has no such preference, so we keep it enabled.
  constexpr bool kStarfieldMotionEnabled = true;
  // gh.data _DAT_00575738 = 0.01 (double): parallax speed multiplier, so
  // speed = NovaRandom_Range(0x23) * 0.01 spans 0.00..0.34.
  constexpr float kSpeedScale = 0.01F;
  for (int i = 0; i < 20; ++i) {
    AmbientStar &s = ambient_stars_[static_cast<std::size_t>(i)];
    s.active = true;
    if (i >= count) {
      // Ghidra: slots beyond the computed count are only re-activated; their
      // previous position/speed carry over.
      continue;
    }
    // Random world offset within the (player-centred) viewport.
    const auto rx = static_cast<float>(NovaRandomRange(state.rng, kViewportWidth));
    const auto ry = static_cast<float>(NovaRandomRange(state.rng, kViewportHeight));
    s.pos_x = rx + state.player.pos_x - static_cast<float>(kViewportWidth) / 2.0F;
    s.pos_y = ry + state.player.pos_y - static_cast<float>(kViewportHeight) / 2.0F;
    // Ghidra: speed = NovaRandom_Range(0x23) * 0.01 when the motion toggle is on,
    // else forced to 0 (stationary field).
    s.speed = kStarfieldMotionEnabled
                  ? static_cast<float>(NovaRandomRange(state.rng, 0x23)) * kSpeedScale
                  : 0.0F;
  }
}

// Ghidra NovaEffects_UpdateAmbientStarParticles (0x0046ee50). Each active
// particle's world position grows by (dx, dy) * per-particle speed, where the
// passed delta is the ship's movement this frame. Only particles whose speed
// clears the drift threshold (gh.data _DAT_005757d8 = 0.0f: any non-zero speed)
// move, mirroring the original's gate.
void SpaceflightView::UpdateAmbientStars(float dx, float dy) {
  constexpr float kMinSpeed = 0.0F; // original gate DAT_005757d8 = 0.0f
  for (auto &s : ambient_stars_) {
    if (s.active && s.speed > kMinSpeed) {
      s.pos_x += dx * s.speed;
      s.pos_y += dy * s.speed;
    }
  }
}

// Draws the solid per-system space backdrop (a flat tint from SystemDef
// BkgndColor, pure black when unset) and then the active ambient star particles.
// Ghidra: Frame_RenderViewportBackground clears + fills with the
// NovaRender_SetSystemSpaceBackgroundColor tint, then Frame_UpdateViewportWrapBackgroundSprites
// (+ the sprite-world draw in Frame_SpaceflightLoop scope 2) renders the stars.
// NOTE(decomp): star artwork is a small point approximation rather than the
// original's per-frame sprite sheet (source sheet id not yet located).
void SpaceflightView::DrawBackground(SDL_Renderer *renderer,
                                     const GameState &state) {
  // Per-system space background tint. Ghidra NovaRender_SetSystemSpaceBackgroundColor
  // uses SystemDef.field_0x1ee (the decoded RGB bytes); RRGGBB = 0 is pure
  // black. This replaces the earlier provisional government-theme wash.
  const auto *sys = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  const std::uint32_t c = sys ? sys->bkgnd_color : 0;
  const std::uint8_t bg_r = static_cast<std::uint8_t>((c >> 16) & 0xff);
  const std::uint8_t bg_g = static_cast<std::uint8_t>((c >> 8) & 0xff);
  const std::uint8_t bg_b = static_cast<std::uint8_t>(c & 0xff);
  SDL_SetRenderDrawColor(renderer, bg_r, bg_g, bg_b, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);

  // Star size: the original sizes each star sprite by 0x20 (32) when murk==0,
  // else scales by round(murk * 0.9), clamped to [2,29]. Mirrors
  // Frame_UpdateViewportWrapBackgroundSprites (gh.data _DAT_005753c0 = 0.9).
  std::int16_t star = 32;
  if (sys && sys->murk != 0) {
    star = static_cast<std::int16_t>(
        std::round(static_cast<float>(sys->murk) * 0.9F));
    if (star > 29) star = 29;
    if (star < 2) star = 2;
  }
  star_size_ = star;

  // Ambient star particles, wrapped around the current viewport (Ghidra
  // Frame_UpdateViewportWrapBackgroundSprites relocates an off-edge particle to
  // the opposite edge). When murk hides them we already cleared on spawn, so
  // nothing is drawn. Otherwise each active star is drawn as a small point
  // behind the world.
  for (const auto &s : ambient_stars_) {
    if (!s.active) {
      continue;
    }
    const float sx = (s.pos_x - state.player.pos_x) + kViewportWidth / 2;
    const float sy = (s.pos_y - state.player.pos_y) + kViewportHeight / 2;
    float wx = std::fmod(sx, static_cast<float>(kViewportWidth));
    float wy = std::fmod(sy, static_cast<float>(kViewportHeight));
    if (wx < 0.0F) wx += static_cast<float>(kViewportWidth);
    if (wy < 0.0F) wy += static_cast<float>(kViewportHeight);
    // Provisional brightness/size scaling off the per-particle speed so nearer
    // (faster) stars read brighter, and the murk-derived star_size_ so gloomy
    // systems show heavier stars (the original scales each star sprite by the
    // same murk-derived value).
    const float bright = 180.0F + 75.0F * std::clamp(s.speed, 0.0F, 1.0F);
    SDL_SetRenderDrawColor(renderer, static_cast<std::uint8_t>(bright),
                           static_cast<std::uint8_t>(bright),
                           static_cast<std::uint8_t>(std::min(255.0F, bright + 40.0F)),
                           SDL_ALPHA_OPAQUE);
    if (star_size_ <= 0) {
      SDL_RenderPoint(renderer, wx, wy);
    } else {
      const float d = static_cast<float>(star_size_) * 0.0625F;  // ~2px at scale 32
      const SDL_FRect r{wx - d, wy - d, d * 2.0F, d * 2.0F};
      SDL_RenderFillRect(renderer, &r);
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
  // The game renders only the current system's owned space objects: the
  // NavDef1-16 list (System.nav_defs, payload +0x24) holds the stellar resource
  // ids that belong to this system (Kania owns Port Kane + the hypergate). Any
  // other stellar in the global table belongs to a different system and must
  // not be drawn here.
  for (const auto nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const auto *st = state.scenario.Stellar(nav);
    if (!st || st->name.empty()) {
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
  DrawBackground(renderer, state);
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
