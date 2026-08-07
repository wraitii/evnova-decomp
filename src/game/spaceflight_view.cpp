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
// Fixed logical play area reserved at the bottom of the window for the HUD
// strip (the placeholder HUD sits at y 400..460). At the default 640x480 window
// the space viewport is the 640x400 region above it.
constexpr int kViewportWidth = 640;
constexpr int kViewportHeight = 400;

// The current space-viewport size, in logical (1:1) pixels. In resolution-
// extension mode the world "extends": a larger window shows more of the system,
// so the camera, the star-field simulation and the culling all track the full
// window size (the HUD is a placeholder overlay drawn over the bottom band). In
// scale-to-window mode the viewport is the fixed 640x400 playfield above the
// HUD reserve.
struct Viewport {
  int w = kViewportWidth;
  int h = kViewportHeight;
};

[[nodiscard]] Viewport CurrentViewport(const SdlPlatform &platform) {
  const auto sz = platform.logical_playfield_size();
  const int w = std::max(kViewportWidth, static_cast<int>(sz.x));
  const int h = std::max(kViewportHeight, static_cast<int>(sz.y));
  return {w, h};
}

// Frame index for a heading. EV Nova ships point 'up' at frame 0 with heading
// increasing clockwise; frames progress one per sector of the rotation.
// TODO(decomp): verify phase/clockwise orientation against a real rendered
// ship -- this is the conventional mapping and should be re-checked once the
// ship is on screen.
[[nodiscard]] int FrameForHeading(float heading_radians,
                                  int frames_per_rotation) {
  const float normalized = std::fmod(heading_radians + kTwoPi, kTwoPi);
  const float sector =
      (normalized / kTwoPi) * static_cast<float>(frames_per_rotation);
  int frame = static_cast<int>(std::lround(sector)) % frames_per_rotation;
  if (frame < 0) {
    frame += frames_per_rotation;
  }
  return frame;
}

// Uniform integer in [0, bound). Mirrors the game's NovaRandom_Range seeded
// from the GameState PRNG so the spawn layout is reproducible per session.
[[nodiscard]] std::int16_t NovaRandomRange(std::mt19937 &rng, int bound) {
  if (bound <= 1) {
    return 0;
  }
  return static_cast<std::int16_t>(
      std::uniform_int_distribution<int>{0, bound - 1}(rng));
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
  SDL_Renderer *const renderer = platform.renderer();
  auto base = LoadShipSprite(renderer, visual->base_image_id);
  if (!base) {
    NovaLog::Warn("ship sprite: no usable rl.x91D sheet {} for class '{}'",
                  visual->base_image_id,
                  ship_class->display_name);
    return false;
  }
  ship_ = std::move(*base);
  ship_.frames_per_rotation = visual->frames_per_rotation;

  // Engine-glow layer (GlowImageID). The original loads it into the per-class
  // glow sprite set sharing the base's rotation grid (same frame count, set by
  // frames_per_rotation * base_set_count); it is drawn over the base with a
  // thrust-driven alpha (see Draw). A missing/absent glow sheet just means the
  // ship has no glow layer, which is not fatal.
  has_glow_ = visual->engine_glow_image_id > 0;
  if (has_glow_) {
    if (auto glow = LoadShipSprite(
            renderer, static_cast<std::uint16_t>(visual->engine_glow_image_id));
        glow) {
      glow_ = std::move(*glow);
      glow_.frames_per_rotation = visual->frames_per_rotation;
      NovaLog::Info("ship engine glow loaded for '{}': {}x{} x{} frames",
                    ship_class->display_name,
                    glow_.width,
                    glow_.height,
                    glow_.frame_count);
    } else {
      has_glow_ = false;
      NovaLog::Warn("ship sprite: no usable rl.x91D glow sheet {} for class "
                    "'{}'; engine glow skipped",
                    visual->engine_glow_image_id,
                    ship_class->display_name);
    }
  }

  NovaLog::Info("ship sprite loaded for '{}': {}x{} x{} frames ({} set(s))",
                ship_class->display_name,
                ship_.width,
                ship_.height,
                ship_.frame_count,
                visual->base_set_count);
  return true;
}

std::optional<SpaceflightView::ShipSprite>
SpaceflightView::LoadShipSprite(SDL_Renderer *renderer,
                                std::uint16_t resource_id) {
  const auto sheet_data =
      NovaResource_Load(kResourceTypeRleSheet16, resource_id);
  if (!sheet_data) {
    return std::nullopt;
  }
  auto sheet = RleSpriteSheet_Decode16(*sheet_data);
  if (!sheet) {
    return std::nullopt;
  }
  ShipSprite out;
  out.frame_count = static_cast<int>(sheet->frames.size());
  out.width = sheet->width;
  out.height = sheet->height;
  out.frames.reserve(sheet->frames.size());
  for (const auto &frame : sheet->frames) {
    auto texture = SdlTexture::Create(
        renderer, sheet->width, sheet->height, frame.rgba_pixels);
    if (!texture) {
      out.frames.clear();
      return std::nullopt;
    }
    out.frames.push_back(std::move(texture));
  }
  return out;
}

// Loads (and caches) the spin sprite set for a stellar's graphic. spin_set_id
// is the stellar's link_a_id; the sp\x9an descriptor id is spin_set_id + 1000
// (the stellar-object spin id range), and its SpritesID names the rl\x91D
// sheet.
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
    return spin_sets_[index].get(); // cached (possibly a failed load -> null)
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
            auto texture = SdlTexture::Create(platform.renderer(),
                                              sheet->width,
                                              sheet->height,
                                              frame.rgba_pixels);
            if (!texture) {
              break;
            }
            owner->frames.push_back(std::move(texture));
          }
          NovaLog::Info("spin sprite id {}: {}x{} {} frames",
                        spin_id,
                        sheet->width,
                        sheet->height,
                        owner->frame_count);
        }
      }
    }
  }
  if (owner->frames.empty()) {
    NovaLog::Warn(
        "no spin sprite for stellar set {} (spin id {})", spin_set_id, spin_id);
    return nullptr; // not cached; caller falls back to tinted disc
  }
  spin_sets_[index] = std::move(owner);
  return spin_sets_[index].get();
}

// Loads (and caches) the ambient star-field artwork: sp\x9an spin descriptor
// resource 700 is a 4x4 grid of 5x5px star tiles (16 shapes), whose SpritesID
// names the rl\x91D sheet. Ghidra builds this into DAT_00593efc via
// Spin_ReadDescriptor(700,..); the spawn picks a random frame in
// [0, frameCount) from it. Falls back to null (plain-point stars) when absent.
const SpaceflightView::StarFieldSheet *
SpaceflightView::EnsureStarFieldSheet(SdlPlatform &platform) {
  if (!star_field_.frames.empty()) {
    return &star_field_; // cached
  }
  // Spin descriptor resource id for the ambient star field (Ghidra
  // Spin_ReadDescriptor(700,..)); a sp\x9an descriptor, distinct from the
  // stellar spin-object range (+1000).
  constexpr std::uint16_t kStarFieldSpinId = 700;
  const auto spin_data =
      NovaResource_Load(kResourceTypeSprites, kStarFieldSpinId);
  if (!spin_data) {
    NovaLog::Warn("star field: no sp.x9an descriptor resource {}; stars drawn "
                  "as points",
                  kStarFieldSpinId);
    return nullptr;
  }
  const auto def = NovaSpriteDefinition_Parse(*spin_data);
  if (!def) {
    NovaLog::Warn("star field: malformed sp.x9an descriptor {}; stars drawn "
                  "as points",
                  kStarFieldSpinId);
    return nullptr;
  }
  const auto sheet_data =
      NovaResource_Load(kResourceTypeRleSheet16, def->sprites_resource_id);
  if (!sheet_data) {
    NovaLog::Warn("star field: no rl.x91D sheet {}; stars drawn as points",
                  def->sprites_resource_id);
    return nullptr;
  }
  auto sheet = RleSpriteSheet_Decode16(*sheet_data);
  if (!sheet || def->tile_width <= 0 || def->tile_height <= 0 ||
      sheet->width != def->tile_width || sheet->height != def->tile_height) {
    NovaLog::Warn("star field: rl.x91D sheet {} not a valid tile grid; stars "
                  "drawn as points",
                  def->sprites_resource_id);
    return nullptr;
  }
  star_field_.frame_count = static_cast<int>(sheet->frames.size());
  star_field_.tile_width = sheet->width;
  star_field_.tile_height = sheet->height;
  SDL_Renderer *const renderer = platform.renderer();
  star_field_.frames.reserve(sheet->frames.size());
  for (const auto &frame : sheet->frames) {
    auto texture = SdlTexture::Create(
        renderer, sheet->width, sheet->height, frame.rgba_pixels);
    if (!texture) {
      star_field_.frames.clear();
      NovaLog::Warn("star field: texture upload failed; stars drawn as points");
      return nullptr;
    }
    // The 5x5 tiles are tiny; upscale smoothly so scaled-up stars look like
    // soft glows rather than chunky squares (matches the original's smooth
    // sprite scaling draw-proc).
    SDL_SetTextureScaleMode(texture->get(), SDL_SCALEMODE_LINEAR);
    star_field_.frames.push_back(std::move(texture));
  }
  NovaLog::Info("star field sheet loaded: {}x{} {} frames",
                star_field_.tile_width,
                star_field_.tile_height,
                star_field_.frame_count);
  return &star_field_;
}

// Ghidra NovaEffects_QueuedAmbientStarParticles (0x0046ebf0). See the header.
// Decoded from the binary: spawn count = round(viewportHeight / 600.0 * 20.0)
// (divisor g_background_star_spawn_height_divisor = 600.0). Each of the first
// `count` slots gets a random world offset within the (player-centred) viewport
// and a parallax speed of NovaRandom_Range(0x23) * 0.01 (constant
// _DAT_00575738, a double). The remaining (20-count) slots are merely
// re-activated, keeping their previous position/speed (a re-scatter only
// rewrites the first `count`). A negative system murk (SystemDef.murk) clears
// the whole field. When the per-gameplay options toggle DAT_005914d7 is clear
// (starfield motion disabled) the speed is forced to zero so the field is
// static. The star-field sprite sheet (sp\x9an 700) is ensured loaded here so
// the per-star frame index bound (frame count) is known.
void SpaceflightView::SpawnAmbientStars(SdlPlatform &platform,
                                        GameState &state) {
  // Load the star artwork (if not already) so the frame-count bound is known;
  // a missing sheet just leaves stars as plain points.
  (void)EnsureStarFieldSheet(platform);
  const Viewport vp = CurrentViewport(platform);
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
  const int count = std::clamp<int>(
      static_cast<int>(std::round(vp.h / 600.0F * 20.0F)), 0, 20);
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
    // Star sprite frame index: the original picks NovaRandom_Range(frameCount)
    // uniformly over the star sheet's 16 tiles (Ghida DAT_00593efc +0x54).
    // When the sheet is unavailable we keep frame 0 (fallback point draw).
    const int frame_count =
        star_field_.frame_count > 0 ? star_field_.frame_count : 1;
    s.frame = NovaRandomRange(state.rng, frame_count);
    // Random world offset within the (player-centred) viewport.
    const auto rx = static_cast<float>(NovaRandomRange(state.rng, vp.w));
    const auto ry = static_cast<float>(NovaRandomRange(state.rng, vp.h));
    s.pos_x = rx + state.player.pos_x - static_cast<float>(vp.w) / 2.0F;
    s.pos_y = ry + state.player.pos_y - static_cast<float>(vp.h) / 2.0F;
    // Ghidra: speed = NovaRandom_Range(0x23) * 0.01 when the motion toggle is
    // on, else forced to 0 (stationary field).
    s.speed =
        kStarfieldMotionEnabled
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
// BkgndColor, pure black when unset) and then the active ambient star
// particles. Ghidra: Frame_RenderViewportBackground clears + fills with the
// NovaRender_SetSystemSpaceBackgroundColor tint, then
// Frame_UpdateViewportWrapBackgroundSprites
// (+ the sprite-world draw in Frame_SpaceflightLoop scope 2) renders the stars.
// Each star draws its randomly-chosen frame of the 16-frame star-field sprite
// sheet (sp\x9an 700, 4x4 grid of 5x5 tiles) at native 1:1 size; a small point
// stands in only if the sheet cannot load.
void SpaceflightView::DrawBackground(SdlPlatform &platform,
                                     const GameState &state) {
  SDL_Renderer *const renderer = platform.renderer();
  // Per-system space background tint. Ghidra
  // NovaRender_SetSystemSpaceBackgroundColor uses SystemDef.field_0x1ee (the
  // decoded RGB bytes); RRGGBB = 0 is pure black. This replaces the earlier
  // provisional government-theme wash.
  const auto *sys = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  const std::uint32_t c = sys ? sys->bkgnd_color : 0;
  const std::uint8_t bg_r = static_cast<std::uint8_t>((c >> 16) & 0xff);
  const std::uint8_t bg_g = static_cast<std::uint8_t>((c >> 8) & 0xff);
  const std::uint8_t bg_b = static_cast<std::uint8_t>(c & 0xff);
  SDL_SetRenderDrawColor(renderer, bg_r, bg_g, bg_b, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);

  // Ambient star particles, wrapped around the current viewport (Ghidra
  // Frame_UpdateViewportWrapBackgroundSprites relocates an off-edge particle to
  // the opposite edge). When murk hides them we already cleared on spawn, so
  // nothing is drawn.
  // Each star is one tile of the 5px star-field sprite sheet (sp\x9an 700)
  // drawn at its native pixel size. The 0x20 / murk-derived value the ORIGINAL
  // writes into the sprite's +0xa2..0xa8 fields is a BLEND-CODE sentinel (0x20
  // = raw/tinted-raw blit; round(murk*0.9) in [2,29] selects a hazy tinted/
  // indexed blend), not a pixel dimension - so each star stays ~5px regardless.
  const Viewport vp = CurrentViewport(platform);
  const StarFieldSheet *sheet = EnsureStarFieldSheet(platform);
  for (const auto &s : ambient_stars_) {
    if (!s.active) {
      continue;
    }
    const float sx = (s.pos_x - state.player.pos_x) + vp.w / 2;
    const float sy = (s.pos_y - state.player.pos_y) + vp.h / 2;
    float wx = std::fmod(sx, static_cast<float>(vp.w));
    float wy = std::fmod(sy, static_cast<float>(vp.h));
    if (wx < 0.0F)
      wx += static_cast<float>(vp.w);
    if (wy < 0.0F)
      wy += static_cast<float>(vp.h);

    if (sheet && !sheet->frames.empty()) {
      const int frame_idx = std::clamp(s.frame, 0, sheet->frame_count - 1);
      const auto &texture = sheet->frames[static_cast<std::size_t>(frame_idx)];
      // Native tile size: draw the 5x5 star 1:1, centred on its position.
      const float tw = static_cast<float>(sheet->tile_width);
      const float th = static_cast<float>(sheet->tile_height);
      const SDL_FRect dest{wx - tw / 2.0F, wy - th / 2.0F, tw, th};
      SDL_RenderTexture(renderer, texture->get(), nullptr, &dest);
      continue;
    }

    // Fallback when the star-field sheet is unavailable: a small single-pixel
    // point (matches a 5px sprite scaled for the 400px viewport).
    SDL_RenderPoint(renderer, wx, wy);
  }
}

// Ghidra Stellar_UpdateStellarSprites (0x0042cd10), ambient-animation part.
// Advances each animate stellar of the current system one animation step each
// real frame. Stellar bodies with only a single frame (sprite_frame_count<2)
// stay static at frame 0. For ordinary animated stellars (availability_flags &
// 0x1000 clear) this is the ping-pong/alternate/random cycling stepper; for
// hypergate-style stellars (0x1000 set) it is the non-engaged frame-drift
// toward/around engage_highlight_frame. Engage-highlight pulsing needs AI ship
// states and travel-selection (g_travel_selected_stellar_id / g_travel_engage_
// timer), which this build does not model, so that leg is documented as a
// no-op and only the non-engaged drift is applied. frame_time_ms mirrors the
// original's _g_avg_frame_time_ms accumulator basis.
void SpaceflightView::AdvanceStellarAnimation(SdlPlatform &platform,
                                              GameState &state,
                                              float frame_time_ms) {
  const auto *sys = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (!sys) {
    return;
  }
  for (const auto nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const auto *st = state.scenario.Stellar(nav);
    if (!st || st->name.empty()) {
      continue;
    }
    // The original builds the active spin set from link_a (primary) when the
    // stellar is not engaged/hand-over, link_b otherwise; every starting-system
    // stellar has link_b == -1, so the primary link_a set is the one animated.
    const auto *set = GetSpinSpriteSet(platform, st->link_a_id);
    if (!set || set->frame_count < 2) {
      continue; // no animated set / single-frame body stays static
    }
    StellarAnimState &anim = stellar_anims_[nav];
    const int frame_count = set->frame_count;

    if ((st->availability_flags & 0x1000) == 0) {
      // ---- Ordinary ambient animation (no hypergate-style highlighting). ----
      anim.frame_accumulator += frame_time_ms;
      // Dwell: hold frame 0 longer (Bible Frame0Bias) when it is shown and the
      // multiplier is set.
      const int dwell =
          (anim.current_frame == 0 && st->animation_frame_multiplier > 1)
              ? st->animation_dwell_time * st->animation_frame_multiplier
              : st->animation_dwell_time;
      if (dwell <= anim.frame_accumulator) {
        anim.frame_accumulator = 0.0F;
        if ((st->availability_flags & 1) == 0) {
          // Alternate: current tracks previous; previous walks forward (or
          // jumps to a random frame != current when the random bit is set).
          anim.current_frame = anim.previous_frame;
          if ((st->availability_flags & 2) == 0) {
            anim.previous_frame = (anim.previous_frame + 1) % frame_count;
          } else {
            do {
              anim.previous_frame = NovaRandomRange(state.rng, frame_count);
            } while (anim.previous_frame == anim.current_frame);
          }
        } else if (anim.current_frame == 0) {
          // Linger-on-0: leave frame 0, show previous, then advance it (never
          // returning it to 0 in the sequential case; random skips 0).
          anim.current_frame = anim.previous_frame;
          if ((st->availability_flags & 2) == 0) {
            anim.previous_frame = (anim.previous_frame + 1) % frame_count;
            if (anim.previous_frame == 0) {
              anim.previous_frame += 1;
            }
          } else {
            do {
              do {
                anim.previous_frame = NovaRandomRange(state.rng, frame_count);
              } while (anim.previous_frame == 0);
            } while (anim.previous_frame == anim.current_frame);
          }
        } else {
          // (flags & 1) with a non-zero current frame: snap back to frame 0.
          anim.current_frame = 0;
        }
      }
    } else {
      // ---- Hypergate-style stellar (availability_flags & 0x1000): ----------
      // Drift frames toward/around the engage_highlight_frame (default middle).
      int highlight = st->engage_highlight_frame;
      if (highlight < 1 || frame_count - 1 <= highlight) {
        highlight = frame_count / 2; // Ghidra (frame_count+1US -1)>>1 == fc/2
      }
      // TODO(decomp): g_travel_selected_stellar_id / g_travel_engage_timer and
      // active-ship engagement not modeled here; the engaged pulse-to-highlight
      // leg is skipped (no travel-selection / AI ship state in this build). The
      // non-engaged drift below reproduces the original exactly for this case.
      if (st->animation_dwell_time <= anim.frame_accumulator) {
        anim.frame_accumulator = 0.0F;
        const int cur = anim.current_frame;
        if (cur < highlight) {
          if (cur > 0) {
            anim.current_frame = cur - 1; // drift down toward 0 / highlight
          }
        } else if ((st->availability_flags & 2) == 0) {
          if (cur < frame_count - 1) {
            anim.current_frame = cur + 1;
          } else {
            anim.current_frame = highlight - 1;
          }
        } else {
          anim.current_frame = highlight - 1;
        }
      }
    }
  }
}

// Draws the current system's stellar bodies. When the body's spin sprite set
// (link_a_id -> sp\x9an id+1000) loads, its real planet art is drawn; otherwise
// a tinted circle stands in. Positions are world px offset by the player
// (camera centred on the ship). Animated stellars draw their per-frame advanced
// frame (see AdvanceStellarAnimation).
void SpaceflightView::DrawStellarBodies(SdlPlatform &platform,
                                        const GameState &state) {
  SDL_Renderer *const renderer = platform.renderer();
  const auto *sys = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (!sys) {
    return;
  }
  // The game renders only the current system's owned space objects: the
  // NavDef1-16 list (System.nav_defs, payload +0x24) holds the stellar resource
  // ids that belong to this system (Kania owns Port Kane + the hypergate). Any
  // other stellar in the global table belongs to a different system and must
  // not be drawn here.
  const Viewport vp = CurrentViewport(platform);
  for (const auto nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const auto *st = state.scenario.Stellar(nav);
    if (!st || st->name.empty()) {
      continue;
    }
    const int cx =
        (st->pos_x - static_cast<int>(state.player.pos_x)) + vp.w / 2;
    const int cy =
        (st->pos_y - static_cast<int>(state.player.pos_y)) + vp.h / 2;
    if (cx < -160 || cx > vp.w + 160 || cy < -160 || cy > vp.h + 160) {
      continue; // off-screen
    }
    // Pick an 8-bit tint: the stellar's government (now decoded) if present,
    // else the system government.
    std::uint8_t r = 170, g = 170, b = 200;
    const auto *gov = st->government_id >= 0x80
                          ? state.scenario.Government(st->government_id)
                          : state.scenario.Government(sys->government_id);
    if (gov && gov->present) {
      r = gov->theme_red;
      g = gov->theme_green;
      b = gov->theme_blue;
    }

    // Prefer the real spin planet sprite; fall back to a tinted disc. Animated
    // stellars use the frame advanced by AdvanceStellarAnimation (keyed by the
    // stellar id); single-frame bodies stay at frame 0.
    const auto *set = GetSpinSpriteSet(platform, st->link_a_id);
    if (set && !set->frames.empty()) {
      int frame_idx = 0;
      const auto anim_it = stellar_anims_.find(nav);
      if (anim_it != stellar_anims_.end()) {
        frame_idx =
            std::clamp(anim_it->second.current_frame, 0, set->frame_count - 1);
      }
      const auto &frame = set->frames[static_cast<std::size_t>(frame_idx)];
      const float scale = 1.0F; // planets render at native world size
      const SDL_FRect dest{
          static_cast<float>(cx) - set->tile_width * scale / 2.0F,
          static_cast<float>(cy) - set->tile_height * scale / 2.0F,
          set->tile_width * scale,
          set->tile_height * scale};
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
  DrawBackground(platform, state);
  DrawStellarBodies(platform, state);

  // Player ship at the play-area centre, frame selected by heading.
  if (!ship_.frames.empty() && ship_.frames_per_rotation > 0) {
    const Viewport vp = CurrentViewport(platform);
    const float cx = static_cast<float>(vp.w) / 2.0F;
    const float cy = static_cast<float>(vp.h) / 2.0F;
    const int frame =
        FrameForHeading(state.player.heading, ship_.frames_per_rotation);
    const int clamped_frame = std::clamp(frame, 0, ship_.frame_count - 1);
    const auto &texture = ship_.frames[static_cast<std::size_t>(clamped_frame)];
    const float scale = 1.0F; // original draws ship at native size
    const float w = static_cast<float>(ship_.width) * scale;
    const float h = static_cast<float>(ship_.height) * scale;
    const SDL_FRect dest{cx - w / 2.0F, cy - h / 2.0F, w, h};
    SDL_RenderTexture(renderer, texture->get(), nullptr, &dest);

    // Engine-glow layer: drawn over the base with the same heading-selected
    // frame and a thrust-driven alpha. The original binds the glow as a second
    // sprite layer on top of the base set to the same frame index (Ghidra
    // NovaUi_UpdateShipClassLaunchProgress sets both to sVar10). The glow
    // sheet is larger than the base (exhaust jets extend beyond the hull), so
    // it is drawn centred on the ship at its native size. Alpha = the ramped
    // thrust intensity, so the exhaust fades in while accelerating and out when
    // coasting (clean-room approximation of the original dimming the glow with
    // throttle).
    if (has_glow_ && state.player.engine_glow_intensity > 0.0F &&
        !glow_.frames.empty() && glow_.frames_per_rotation > 0) {
      if (!glow_last_drawn_) {
        NovaLog::Info("[glow] draw ON intensity={:.2f} frames={} size={}x{}",
                      state.player.engine_glow_intensity,
                      glow_.frame_count,
                      glow_.width,
                      glow_.height);
        glow_last_drawn_ = true;
      }
      const int glow_frame = std::clamp(
          FrameForHeading(state.player.heading, glow_.frames_per_rotation),
          0,
          glow_.frame_count - 1);
      const auto &glow_texture =
          glow_.frames[static_cast<std::size_t>(glow_frame)];
      const float gw = static_cast<float>(glow_.width);
      const float gh = static_cast<float>(glow_.height);
      const SDL_FRect glow_dest{cx - gw / 2.0F, cy - gh / 2.0F, gw, gh};
      const std::uint8_t alpha = static_cast<std::uint8_t>(
          std::clamp(state.player.engine_glow_intensity, 0.0F, 1.0F) * 255.0F);
      SDL_SetTextureAlphaMod(glow_texture->get(), alpha);
      SDL_RenderTexture(renderer, glow_texture->get(), nullptr, &glow_dest);
      SDL_SetTextureAlphaMod(glow_texture->get(), SDL_ALPHA_OPAQUE);
    } else if (glow_last_drawn_) {
      // Won't draw this frame (intensity leaked below the gate / layer empty).
      NovaLog::Info("[glow] draw OFF has_glow={} intensity={:.2f} frames={}",
                    has_glow_,
                    state.player.engine_glow_intensity,
                    glow_.frame_count);
      glow_last_drawn_ = false;
    }
  }
}

} // namespace game
