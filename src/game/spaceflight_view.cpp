#include "spaceflight_view.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../rle_sprite_sheet.hpp"
#include "../sdl_platform.hpp"
#include "game_state.hpp"
#include "hud_renderer.hpp"
#include "impact_effects.hpp"
#include "ship_ai.hpp"
#include "ship_visual.hpp"
#include "targeting.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <tuple>

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

// During the jump the ship stays screen-centred; the camera is the plain
// player position (the tunnel ramp drives the ship's own position, so the
// camera ride comes for free).
std::pair<float, float> WorldCameraPosition(const GameState &state) {
  return {state.player.pos_x, state.player.pos_y};
}

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

// Ghidra Ship_UpdateVisualState (0x00428340) frame composition: the displayed
// index is row * frames_per_rotation + heading_frame. Row 0 is the straight-
// flight rotation grid; for banking classes (sh\x8an Flags & 1) the sprite's
// alternate rows show the bank-left (row 1) / bank-right (row 2) cycle chosen
// by ai_turn_bias_dir (+0xc8f8). The row is clamped to the loaded rows so a
// class without an alt sheet falls back to row 0.
[[nodiscard]] int ComposeShipFrameIndex(const Ship &ship,
                                        std::uint16_t sprite_behavior_flags,
                                        int frames_per_rotation,
                                        int row_count) {
  int row = 0;
  if ((sprite_behavior_flags & 1U) != 0U) {
    if (ship.ai_turn_bias_dir < 0) {
      row = 1;
    } else if (ship.ai_turn_bias_dir > 0) {
      row = 2;
    }
  }
  row = std::clamp(row, 0, std::max(1, row_count) - 1);
  return row * frames_per_rotation +
         FrameForHeading(ship.heading, frames_per_rotation);
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

// ---- beam rendering (Ghidra SWBeams.c family) -----------------------------

[[nodiscard]] std::tuple<std::uint8_t, std::uint8_t, std::uint8_t>
SplitRgb(std::uint32_t packed) {
  return {static_cast<std::uint8_t>(packed >> 16 & 0xffU),
          static_cast<std::uint8_t>(packed >> 8 & 0xffU),
          static_cast<std::uint8_t>(packed & 0xffU)};
}

// Ghidra Beam_DrawBlendedSegment (0x00479590) + Beam_BlendLineSegment
// (0x004797b0) alpha profile, re-expressed for SDL. The original rasterizes a
// 15-bit Bresenham line, blending the beam colour over the saved backdrop at a
// per-pixel fade on a 0..0x20 scale; here the same profile drives 8-bit-alpha
// SDL points (alpha*255/32 matches the original alpha/32 linear blend).
// Per-pixel profile: holds `start_fade`, ramps +1/px up to `plateau` across
// the first `start_ramp` px, then falls 1/px across the final `end_fade` px
// before the far endpoint, stopping when it reaches zero. All inputs clamp to
// 0..0x20 exactly as Beam_DrawBlendedSegment does.
// Divergence: the original blends in RGB555 surface space; SDL blends 8-bit
// RGB channels (higher fidelity, same profile).
void DrawBeamBlendedLine(SDL_Renderer *renderer,
                         int x0,
                         int y0,
                         int x1,
                         int y1,
                         std::uint8_t red,
                         std::uint8_t green,
                         std::uint8_t blue,
                         int plateau,
                         int start_fade,
                         int start_ramp,
                         int end_fade) {
  const auto clamp32 = [](int v) { return std::clamp(v, 0, 0x20); };
  plateau = clamp32(plateau);
  start_fade = clamp32(start_fade);
  start_ramp = clamp32(start_ramp);
  end_fade = clamp32(end_fade);

  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int adx = std::abs(dx);
  const int ady = std::abs(dy);
  const int steps = std::max(adx, ady);
  if (steps == 0) {
    return;
  }

  int alpha = start_ramp > 0 ? start_fade : plateau;
  int x = x0;
  int y = y0;
  const int x_step = dx < 0 ? -1 : 1;
  const int y_step = dy < 0 ? -1 : 1;
  int err = steps / 2;
  for (int step = 0;; ++step) {
    if (alpha <= 0) {
      return;
    }
    SDL_SetRenderDrawColor(renderer,
                           red,
                           green,
                           blue,
                           static_cast<std::uint8_t>(alpha * 255 / 0x20));
    SDL_RenderPoint(renderer, static_cast<float>(x), static_cast<float>(y));
    if (step == steps) {
      return;
    }
    if (adx >= ady) {
      x += x_step;
      err -= ady;
      if (err < 0) {
        y += y_step;
        err += adx;
      }
    } else {
      y += y_step;
      err -= adx;
      if (err < 0) {
        x += x_step;
        err += ady;
      }
    }
    // BlendLineSegment adjusts the fade after each Bresenham step, measuring
    // distance along the dominant axis.
    const int from_start = adx >= ady ? std::abs(x - x0) : std::abs(y - y0);
    if (from_start < start_ramp) {
      if (alpha < plateau) {
        ++alpha;
      }
    } else if (steps - from_start < end_fade) {
      --alpha;
    } else if (alpha != plateau) {
      alpha = plateau;
    }
  }
}

// Ghidra SWBeams_DrawShortBeam (0x00479fe0): core + corona of a straight beam
// between (xa,ya) = muzzle and (xb,yb) = target. Parallel lines are offset
// along the axis perpendicular to the beam, exactly like the original.
// Core pass i (i < width): offsets ±i, plateau alpha 0x20 - 0x10*i, fading out
// over the last 0x14 - 0xa*i px at the target end. Width 0 draws no core
// (Bible: "A BeamWidth of 0 will have no center beam, just corona glow").
// Corona pass k: offsets ±(width + k), plateau 0x10 - k*falloff (drawn while
// > 2), ramping in over 8 - 2k px from the muzzle and out over 8 + 6k px at
// the target; the caller grows falloff by the decay counter as the beam dies.
void DrawBeamCoreAndCorona(SDL_Renderer *renderer,
                           int xa,
                           int ya,
                           int xb,
                           int yb,
                           std::uint32_t core_color,
                           int width,
                           std::uint32_t corona_color,
                           int falloff) {
  const bool horizontal = std::abs(xb - xa) >= std::abs(yb - ya);
  const auto offset_a = [&](int off) -> std::pair<int, int> {
    return horizontal ? std::pair{xa, ya - off} : std::pair{xa - off, ya};
  };
  const auto offset_b = [&](int off) -> std::pair<int, int> {
    return horizontal ? std::pair{xb, yb - off} : std::pair{xb - off, yb};
  };

  // Core.
  const auto [cr, cg, cb] = SplitRgb(core_color);
  for (int i = 0; i < width; ++i) {
    const int alpha = 0x20 - 0x10 * i;
    if (alpha <= 0) {
      break;
    }
    const int end_fade = 0x14 - 0xa * i;
    const auto [ax, ay] = offset_a(i);
    const auto [bx, by] = offset_b(i);
    DrawBeamBlendedLine(
        renderer, ax, ay, bx, by, cr, cg, cb, alpha, alpha, 0, end_fade);
    if (i > 0) {
      const auto [ax2, ay2] = offset_a(-i);
      const auto [bx2, by2] = offset_b(-i);
      DrawBeamBlendedLine(
          renderer, ax2, ay2, bx2, by2, cr, cg, cb, alpha, alpha, 0, end_fade);
    }
  }

  // Corona. The original gates the loop on a positive falloff (a zero/negative
  // value would otherwise never terminate the alpha countdown).
  const auto [kr, kg, kb] = SplitRgb(corona_color);
  if (falloff > 0) {
    int k = 0;
    int alpha = 0x10;
    do {
      const auto [ax, ay] = offset_a(width + k);
      const auto [bx, by] = offset_b(width + k);
      DrawBeamBlendedLine(renderer,
                          ax,
                          ay,
                          bx,
                          by,
                          kr,
                          kg,
                          kb,
                          alpha,
                          8 - 2 * k,
                          2 * k,
                          8 + 6 * k);
      const auto [ax2, ay2] = offset_a(-(width + k));
      const auto [bx2, by2] = offset_b(-(width + k));
      DrawBeamBlendedLine(renderer,
                          ax2,
                          ay2,
                          bx2,
                          by2,
                          kr,
                          kg,
                          kb,
                          alpha,
                          8 - 2 * k,
                          2 * k,
                          8 + 6 * k);
      alpha -= falloff;
      ++k;
    } while (alpha > 2);
  }
}

// Ghidra SWBeams_DrawThickFadingBeam (0x0047a410): lightning-beam plotter.
// Chains round(max(|dx|,|dy|) * density / 2) jittered segments from muzzle to
// target (jitter ±amplitude per axis; the final two steps land on the exact
// target), and for each segment draws parallel lines at perpendicular offsets
// 0..width-1 (mirrored past offset 0) at alpha base - j*0x20/(width+1).
// Only the first segment ending exactly at the target fades, over 0x20 px.
void DrawLightningBeam(SDL_Renderer *renderer,
                       std::mt19937 &rng,
                       int xa,
                       int ya,
                       int xb,
                       int yb,
                       std::uint32_t color,
                       int width,
                       int density,
                       int amplitude,
                       int base_alpha) {
  const int reach = std::max(std::abs(xb - xa), std::abs(yb - ya));
  int steps = static_cast<int>(std::lround(static_cast<float>(reach) *
                                           static_cast<float>(density) * 0.5F));
  steps = std::max(steps, 1);
  const float step_x = static_cast<float>(xb - xa) / steps;
  const float step_y = static_cast<float>(yb - ya) / steps;
  std::uniform_int_distribution<int> jitter{-amplitude, amplitude};

  const bool horizontal = std::abs(xb - xa) >= std::abs(yb - ya);
  const auto [cr, cg, cb] = SplitRgb(color);

  int prev_x = xa;
  int prev_y = ya;
  for (int idx = 0; idx < steps; ++idx) {
    int cur_x;
    int cur_y;
    if (idx >= steps - 2) {
      cur_x = xb;
      cur_y = yb;
    } else {
      cur_x =
          static_cast<int>(std::ceil(xa + step_x * (idx + 1))) + jitter(rng);
      cur_y =
          static_cast<int>(std::ceil(ya + step_y * (idx + 1))) + jitter(rng);
    }
    const int end_fade = idx == steps - 2 ? 0x20 : 0;
    for (int j = 0; j < width; ++j) {
      const int alpha = std::max(1, base_alpha - j * (0x20 / (width + 1)));
      if (horizontal) {
        DrawBeamBlendedLine(renderer,
                            prev_x,
                            prev_y + j,
                            cur_x,
                            cur_y + j,
                            cr,
                            cg,
                            cb,
                            alpha,
                            alpha,
                            0,
                            end_fade);
        if (j > 0) {
          DrawBeamBlendedLine(renderer,
                              prev_x,
                              prev_y - j,
                              cur_x,
                              cur_y - j,
                              cr,
                              cg,
                              cb,
                              alpha,
                              alpha,
                              0,
                              end_fade);
        }
      } else {
        DrawBeamBlendedLine(renderer,
                            prev_x + j,
                            prev_y,
                            cur_x + j,
                            cur_y,
                            cr,
                            cg,
                            cb,
                            alpha,
                            alpha,
                            0,
                            end_fade);
        if (j > 0) {
          DrawBeamBlendedLine(renderer,
                              prev_x - j,
                              prev_y,
                              cur_x - j,
                              cur_y,
                              cr,
                              cg,
                              cb,
                              alpha,
                              alpha,
                              0,
                              end_fade);
        }
      }
    }
    prev_x = cur_x;
    prev_y = cur_y;
  }
}

// Shared per-beam appearance for the two visible beam passes (Ghidra 0x00438c40
// and Shot_DrawBeamHitQueueForSurface 0x00438810, which feed the same SWBeams
// plotters with the same arguments; 0x00438810's twin-surface branches differ
// only by destination surface).
void DrawQueuedBeam(SdlPlatform &platform,
                    std::mt19937 &jitter_rng,
                    const GameState &state,
                    const BeamHit &beam,
                    const Weapon &weapon) {
  const Viewport vp = CurrentViewport(platform);
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  const auto screen_x = [&](float world_x) {
    return static_cast<int>(
        std::lround(world_x - camera_x + static_cast<float>(vp.w) / 2.0F));
  };
  const auto screen_y = [&](float world_y) {
    return static_cast<int>(
        std::lround(world_y - camera_y + static_cast<float>(vp.h) / 2.0F));
  };
  const int xa = screen_x(beam.source_x);
  const int ya = screen_y(beam.source_y);
  const int xb = screen_x(beam.target_x);
  const int yb = screen_y(beam.target_y);
  // WeaponDef +0x72 doubles as Bible BeamWidth for beam modes.
  const int width = weapon.shot_anim_frame_dwell;
  SDL_Renderer *renderer = platform.renderer();
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  if (weapon.beam_lightning_density > 0) {
    DrawLightningBeam(renderer,
                      jitter_rng,
                      xa,
                      ya,
                      xb,
                      yb,
                      weapon.beam_core_color,
                      width,
                      weapon.beam_lightning_density,
                      weapon.beam_lightning_amplitude,
                      0x20 - beam.animation_counter);
  } else {
    DrawBeamCoreAndCorona(renderer,
                          xa,
                          ya,
                          xb,
                          yb,
                          weapon.beam_core_color,
                          width,
                          weapon.beam_corona_color,
                          weapon.beam_falloff + beam.animation_counter);
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// Common queue gate for the visible beam passes: the original skips records
// whose lifetime is below the active sentinel or whose owner slot is unset.
[[nodiscard]] bool BeamRecordVisible(const GameState &state,
                                     const BeamHit &beam) {
  if (beam.lifetime_ticks < -1 || beam.weapon_id < 0 ||
      beam.owner_ship_slot < 0) {
    return false;
  }
  const Weapon *weapon =
      state.scenario.Weapon(static_cast<std::int16_t>(beam.weapon_id + 0x80));
  return weapon != nullptr;
}

} // namespace

bool SpaceflightView::EnsureShipSprite(SdlPlatform &platform,
                                       GameState &state) {
  if (!ship_.frames.empty()) {
    return true;
  }
  const std::int16_t player_class =
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80);
  const auto *ship_class = state.scenario.Ship(player_class);
  if (!ship_class) {
    NovaLog::Warn("ship sprite: no ship class {:#x} in scenario tables",
                  static_cast<unsigned>(player_class));
    return false;
  }
  // The sh\x8an descriptor id matches the ship class id.
  const auto visual_data = NovaResource_Load(
      kShipVisualResourceType, static_cast<std::uint16_t>(player_class));
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
  // Copy the sh\x8an weapon-exit geometry into the player ship so the firing
  // path (NovaWeapon_FirePlayerWeaponBank) can offset shots to the gun barrel.
  for (std::size_t g = 0; g < visual->turret_muzzles.size(); ++g) {
    for (std::size_t q = 0; q < 4; ++q) {
      state.player.muzzle_lateral[g][q] = visual->turret_muzzles[g].lateral[q];
      state.player.muzzle_forward[g][q] = visual->turret_muzzles[g].forward[q];
      state.player.muzzle_drop[g][q] = visual->turret_muzzles[g].drop[q];
    }
    state.player.muzzle_quadrant[g] = -1; // first shot picks a random barrel
  }
  state.player.muzzle_scale_x = visual->muzzle_scale_x;
  state.player.muzzle_scale_y = visual->muzzle_scale_y;
  state.player.muzzle_ready = true;
  SDL_Renderer *const renderer = platform.renderer();
  // Bare rl\x91D ship sheets load through the shared sheet decoder.
  auto base = SpriteAsset::LoadSheet(renderer, visual->base_image_id);
  if (!base) {
    NovaLog::Warn("ship sprite: no usable rl.x91D sheet {} for class '{}'",
                  visual->base_image_id,
                  ship_class->display_name);
    return false;
  }
  ship_ = std::move(*base);
  ship_frames_per_rotation_ = visual->frames_per_rotation;
  ship_sprite_behavior_flags_ = visual->sprite_behavior_flags;
  // NOTE(decomp): the basic sprite sets all live in the SAME rl\x91D sheet
  // (Bible BaseSetCount: "the graphics for all of a ship's basic sprite sets
  // are stored in the same PICT/rleD/rle8 resource"). Verified: the shuttle
  // sheet (rl\x91D 1000) holds 108 frames = 3 rows of 36 (level / bank left /
  // bank right, Bible Flags 0x0001). The sh\x8an AltImageID pair (+0x0c) is a
  // separate alternate sheet only used by the special Flags 0x0002 cycler
  // (Ghidra ShipClass_LoadShipClassVisualAndLaunchData gates it on
  // AltImageID > 0 && AltSetCount > 0) and is not appended to the base rows.
  ship_row_count_ = std::max(1, ship_.frame_count / ship_frames_per_rotation_);

  // Engine-glow layer (GlowImageID). The original loads it into the per-class
  // glow sprite set sharing the base's rotation grid (same frame count, set by
  // frames_per_rotation * base_set_count); it is drawn over the base with a
  // thrust-driven alpha (see Draw). A missing/absent glow sheet just means the
  // ship has no glow layer, which is not fatal.
  has_glow_ = visual->engine_glow_image_id > 0;
  if (has_glow_) {
    if (auto glow = SpriteAsset::LoadSheet(
            renderer, static_cast<std::uint16_t>(visual->engine_glow_image_id));
        glow) {
      glow_ = std::move(*glow);
      glow_.frame_count = ship_.frame_count; // shares the base rotation grid
      NovaLog::Info("ship engine glow loaded for '{}': {}x{} x{} frames",
                    ship_class->display_name,
                    glow_.tile_width,
                    glow_.tile_height,
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
                ship_.tile_width,
                ship_.tile_height,
                ship_.frame_count,
                visual->base_set_count);
  return true;
}

// Loads (and caches) an NPC ship's heading-rotation sheet and engine-glow layer
// for a ship class resource id. This is the general form of EnsureShipSprite's
// base load; the player's call keeps its own glow/muzzle handling on top of the
// same sheet. NPC glow uses the class's GlowImageID (the same sheet the player
// draws), sharing the base's rotation grid; a class with no glow sheet just
// draws base-only.
const SpaceflightView::NpcShipSprite *
SpaceflightView::ShipClassSprite(SdlPlatform &platform,
                                 std::int16_t ship_class_id) {
  const auto cached = npc_ship_sprites_.find(ship_class_id);
  if (cached != npc_ship_sprites_.end()) {
    return &cached->second;
  }

  NpcShipSprite entry;
  const auto visual_data = NovaResource_Load(
      kShipVisualResourceType, static_cast<std::uint16_t>(ship_class_id));
  const auto visual =
      visual_data ? DecodeShipVisualDescriptor(*visual_data) : std::nullopt;
  if (!visual) {
    // Cache a failed entry so a missing class is not retried every frame.
    npc_ship_sprites_[ship_class_id] = std::move(entry);
    return nullptr;
  }
  auto base =
      SpriteAsset::LoadSheet(platform.renderer(), visual->base_image_id);
  if (!base) {
    npc_ship_sprites_[ship_class_id] = std::move(entry);
    return nullptr;
  }
  entry.base = std::move(*base);
  entry.frames_per_rotation = visual->frames_per_rotation;
  entry.sprite_behavior_flags = visual->sprite_behavior_flags;
  // Basic sets all live in the base sheet (see the player-path note in
  // EnsureShipSprite); no alt-sheet append.
  entry.row_count =
      std::max(1, entry.base.frame_count / entry.frames_per_rotation);
  // Engine-glow layer (GlowImageID), sharing the base's rotation grid. An
  // absent glow image id, or a glow sheet that fails to load, just means the
  // NPC has no glow layer (not fatal).
  if (visual->engine_glow_image_id > 0) {
    if (auto glow = SpriteAsset::LoadSheet(
            platform.renderer(),
            static_cast<std::uint16_t>(visual->engine_glow_image_id));
        glow) {
      entry.glow = std::move(*glow);
      entry.glow.frame_count = entry.base.frame_count;
      entry.has_glow = true;
    } else {
      NovaLog::Warn("npc ship sprite: no usable glow sheet {} for class "
                    "{:#x}; engine glow skipped",
                    static_cast<unsigned>(visual->engine_glow_image_id),
                    static_cast<unsigned>(ship_class_id));
    }
  }
  auto [it, inserted] =
      npc_ship_sprites_.emplace(ship_class_id, std::move(entry));
  (void)inserted;
  NovaLog::Info("npc ship sprite loaded for class {:#x}: {}x{} x{} frames{}",
                static_cast<unsigned>(ship_class_id),
                it->second.base.tile_width,
                it->second.base.tile_height,
                it->second.base.frame_count,
                it->second.has_glow ? " + engine glow" : "");
  return &it->second;
}

// Draws every active non-player ship in the current system at its world
// position, frame selected by heading. Filters by system to avoid drawing NPCs
// parked in other systems, and skips ships whose class sprite could not be
// loaded.
void SpaceflightView::DrawNpcShips(SdlPlatform &platform,
                                   const GameState &state) {
  const Viewport vp = CurrentViewport(platform);
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (!ship.is_active ||
        (NovaAiShip_IsDestroyed(ship) &&
         ship.destruction_visual_timer_ms <= 0.0F) ||
        ship.current_system_id != state.player.current_system_id) {
      continue;
    }
    const std::int16_t class_id =
        static_cast<std::int16_t>(ship.ship_class_id + 0x80);
    const NpcShipSprite *sprite = ShipClassSprite(platform, class_id);
    if (sprite == nullptr || sprite->base.frames.empty() ||
        sprite->frames_per_rotation <= 0) {
      continue;
    }
    const int frame = ComposeShipFrameIndex(ship,
                                            sprite->sprite_behavior_flags,
                                            sprite->frames_per_rotation,
                                            sprite->row_count);
    SpriteDrawOptions hull_options;
    if (NovaAiShip_IsDestroyed(ship)) {
      hull_options.alpha_mod =
          std::clamp(ship.destruction_visual_timer_ms / 900.0F, 0.0F, 1.0F) *
          0.55F;
    }
    DrawSprite(platform.renderer(),
               sprite->base,
               frame,
               ship.pos_x,
               ship.pos_y,
               camera_x,
               camera_y,
               vp.w,
               vp.h,
               hull_options);

    // Engine-glow layer, drawn over the hull with the same heading-selected
    // frame and a thrust-driven alpha. engine_glow_level is driven in
    // NovaShip_IntegrateNpcMovement (Ship_HandleShip field_0xc8d4); level/24
    // clamped to [0,1] is the exhaust intensity, matching the player's glow. A
    // class with no glow layer or a currently-dark exhaust just skips this.
    if (sprite->has_glow && !sprite->glow.frames.empty() &&
        ship.engine_glow_intensity > 0.0F) {
      SpriteDrawOptions opts;
      opts.alpha_mod = ship.engine_glow_intensity;
      DrawSprite(platform.renderer(),
                 sprite->glow,
                 frame,
                 ship.pos_x,
                 ship.pos_y,
                 camera_x,
                 camera_y,
                 vp.w,
                 vp.h,
                 opts);
    }
  }
}

// Returns the ambient star-field artwork from the shared sprite store: sp\x9an
// spin descriptor 700 is a 4x4 grid of 5x5px star tiles (16 shapes). Ghidra
// builds this into DAT_00593efc via Spin_ReadDescriptor(700,..); the spawn
// picks a random frame in [0, frameCount) from it. Falls back to null
// (plain-point stars) when absent.
const SpriteAsset *SpaceflightView::StarFieldSheet(SdlPlatform &platform) {
  constexpr std::uint16_t kStarFieldSpinId = 700;
  return sprite_store_.Spin(platform.renderer(), kStarFieldSpinId);
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
  const SpriteAsset *star_sheet = StarFieldSheet(platform);
  const int star_frame_count = star_sheet ? star_sheet->frame_count : 0;
  const Viewport vp = CurrentViewport(platform);
  const auto *sys = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  const bool murk_hides_stars = sys && sys->murk < 0;

  if (murk_hides_stars) {
    // Ghidra NovaEffects_ClearAmbientStarParticles [0x0046ede0]: murk < 0 hides
    // the stars.
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
    const int frame_count = star_frame_count > 0 ? star_frame_count : 1;
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
// particles. Ghidra [0x00497df0]: Frame_RenderViewportBackground clears + fills
// with the NovaRender_SetSystemSpaceBackgroundColor [0x0046bbf0] tint, then
// Frame_UpdateViewportWrapBackgroundSprites [0x0042e590]
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
  const SpriteAsset *sheet = StarFieldSheet(platform);
  for (const auto &s : ambient_stars_) {
    if (!s.active) {
      continue;
    }
    if (sheet && !sheet->frames.empty()) {
      // Native tile size, centred, with one-exit wraparound; linear filtering
      // so the tiny 5x5 star tiles read as soft glows when scaled.
      SpriteDrawOptions opts;
      opts.wrap = true;
      opts.linear_scale = true;
      DrawSprite(renderer,
                 *sheet,
                 s.frame,
                 s.pos_x,
                 s.pos_y,
                 state.player.pos_x,
                 state.player.pos_y,
                 vp.w,
                 vp.h,
                 opts);
      continue;
    }
    // Fallback when the star-field sheet is unavailable: a small single-pixel
    // point (matches a 5px sprite scaled for the 400px viewport).
    const float wx = std::fmod((s.pos_x - state.player.pos_x) + vp.w / 2,
                               static_cast<float>(vp.w));
    const float wy = std::fmod((s.pos_y - state.player.pos_y) + vp.h / 2,
                               static_cast<float>(vp.h));
    SDL_RenderPoint(renderer,
                    wx < 0.0F ? wx + static_cast<float>(vp.w) : wx,
                    wy < 0.0F ? wy + static_cast<float>(vp.h) : wy);
  }
}

// Unified per-frame world animation pass (see the header). One call advances
// every animated in-flight entity's cadence so the timing basis is centralised
// here (Mirror of SpriteWorld_UpdateAnimatedSprites 0x004781f0 at the world
// scope). Drives the animated stellar frame stepping (dwell cadence) and the
// ambient-star parallax movement; time-animated shot frames step in
// NovaWeapon_TickShots using the same dwell model (they need simulation-side
// mutable GameState, so they live there rather than on the SDL view).
void SpaceflightView::AdvanceAnimations(SdlPlatform &platform,
                                        GameState &state,
                                        float frame_time_ms,
                                        float dx,
                                        float dy) {
  // Animated stellar sprite-frame stepping (dwell accumulator cadence).
  AdvanceStellarAnimation(platform, state, frame_time_ms);
  // Impact/debris animation fields are advanced by g_avg_frame_time_ms in the
  // original. Convert SDL milliseconds to the same 30 Hz simulation cadence;
  // passing raw milliseconds would consume a 16-frame explosion in one draw.
  constexpr float kOriginalTickMs = 1000.0F / 30.0F;
  const float elapsed_ticks = frame_time_ms / kOriginalTickMs;
  NovaEffects_TickImpactEffects(state, elapsed_ticks);
  NovaEffects_TickFadingEffects(state, elapsed_ticks);
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    Ship &ship = state.ShipAt(slot);
    const bool was_visible = ship.destruction_visual_timer_ms > 0.0F;
    if (ship.destruction_visual_timer_ms > 0.0F) {
      ship.destruction_visual_timer_ms =
          std::max(0.0F, ship.destruction_visual_timer_ms - frame_time_ms);
    }
    if (was_visible && ship.destruction_visual_timer_ms <= 0.0F &&
        !ship.destruction_finale_triggered) {
      const ShipClass *ship_class = state.scenario.Ship(
          static_cast<std::int16_t>(ship.ship_class_id + 0x80));
      if (ship_class != nullptr && ship_class->destruction_effect_final >= 0) {
        NovaEffects_SpawnShipDestructionFinale(
            state, ship, ship_class->destruction_effect_final);
      }
      ship.destruction_finale_triggered = true;
    }
  }
  // The original impact updater reads each live Sprite's frame count when it
  // decides that an animation has ended. Resolve that SDL-side fact here,
  // after the simulation timer has advanced, so the GameState pool remains
  // independent of renderer handles.
  for (ImpactEffectInstance &instance : state.impact_effect_instances) {
    if (instance.anim_time < 0.0F || instance.delay_timer > 0.0F) {
      continue;
    }
    const ImpactEffect *definition =
        state.scenario.ImpactEffectAt(instance.effect_id);
    if (definition == nullptr) {
      instance.anim_time = -1.0F;
      continue;
    }
    const SpriteAsset *set = sprite_store_.Spin(
        platform.renderer(),
        static_cast<std::uint16_t>(400 + definition->sprite_set_id));
    if (set == nullptr || set->frames.empty() ||
        instance.anim_time >= static_cast<float>(set->frame_count)) {
      instance.anim_time = -1.0F;
    }
  }
  // Ambient-star spatial parallax (moves by the ship's movement delta). The
  // pre-fire tunnel rush is carried by the ship's own position ramp (see
  // TravelState::tunnel_elapsed_60hz in travel.cpp), which this camera parallax
  // follows; TODO(decomp) the original additionally streaks the starfield
  // from the tunnel progress (FLOAT_007354a0 = progress*0.3 - 15.0).
  UpdateAmbientStars(dx, dy);
}

void SpaceflightView::DrawFadingEffects(SdlPlatform &platform,
                                        const GameState &state) {
  const Viewport vp = CurrentViewport(platform);
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  const NpcShipSprite *debris = ShipClassSprite(platform, 0x2ff);
  if (debris == nullptr || debris->base.frames.empty() ||
      debris->frames_per_rotation <= 0) {
    return;
  }
  for (const FadingEffectInstance &fragment : state.fading_effect_instances) {
    if (fragment.lifetime_ticks < 0.0F) {
      continue;
    }
    const int frame =
        FrameForHeading(fragment.heading_radians, debris->frames_per_rotation);
    SpriteDrawOptions options;
    options.alpha_mod =
        std::clamp(fragment.lifetime_ticks / 249.0F, 0.0F, 1.0F);
    DrawSprite(platform.renderer(),
               debris->base,
               frame,
               fragment.pos_x,
               fragment.pos_y,
               camera_x,
               camera_y,
               vp.w,
               vp.h,
               options);
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
    const SpriteAsset *set = sprite_store_.Spin(
        platform.renderer(), static_cast<std::uint16_t>(st->link_a_id + 1000));
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
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  for (const auto nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const auto *st = state.scenario.Stellar(nav);
    if (!st || st->name.empty()) {
      continue;
    }
    const int cx = (st->pos_x - static_cast<int>(camera_x)) + vp.w / 2;
    const int cy = (st->pos_y - static_cast<int>(camera_y)) + vp.h / 2;
    if (cx < -160 || cx > vp.w + 160 || cy < -160 || cy > vp.h + 160) {
      continue; // off-screen
    }
    // Pick an 8-bit tint: the stellar's government (decoded) if present,
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
    // stellar id); single-frame bodies stay at frame 0. Stellar spin sets live
    // at spin resource id link_a_id + 1000 in the shared sprite store.
    const SpriteAsset *set = sprite_store_.Spin(
        platform.renderer(), static_cast<std::uint16_t>(st->link_a_id + 1000));
    if (set && !set->frames.empty()) {
      int frame_idx = 0;
      const auto anim_it = stellar_anims_.find(nav);
      if (anim_it != stellar_anims_.end()) {
        frame_idx =
            std::clamp(anim_it->second.current_frame, 0, set->frame_count - 1);
      }
      DrawSprite(platform.renderer(),
                 *set,
                 frame_idx,
                 static_cast<float>(st->pos_x),
                 static_cast<float>(st->pos_y),
                 camera_x,
                 camera_y,
                 vp.w,
                 vp.h);
      continue;
    }
    // Fallback tinted disc (centred on screen coords like the sprite above).
    const float fsx = (static_cast<float>(st->pos_x) - camera_x) +
                      static_cast<float>(vp.w) / 2.0F;
    const float fsy = (static_cast<float>(st->pos_y) - camera_y) +
                      static_cast<float>(vp.h) / 2.0F;
    const int radius = 14;
    const SDL_FRect rect{fsx - static_cast<float>(radius),
                         fsy - static_cast<float>(radius),
                         static_cast<float>(radius * 2),
                         static_cast<float>(radius * 2)};
    SDL_SetRenderDrawColor(renderer, r / 2U, g / 2U, b / 2U, SDL_ALPHA_OPAQUE);
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, r, g, b, SDL_ALPHA_OPAQUE);
    SDL_RenderRect(renderer, &rect);
  }
}

void SpaceflightView::DrawShots(SdlPlatform &platform, const GameState &state) {
  SDL_Renderer *const renderer = platform.renderer();
  if (state.active_shots.empty()) {
    return;
  }
  const Viewport vp = CurrentViewport(platform);
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  // Each shot uses its weapon's shot sprite set (Ghidra Sprite_AssignSpriteSet
  // on the weapon-sprite-set table entry g_weapon_sprite_set_table
  // [shot_sprite_set_id]; spin resource id shot_sprite_set_id + 3000) from the
  // shared store, drawn at its world position. Shot_HandleShot (0x00435830)
  // does not apply the viewport background's one-exit wrap to projectiles.
  //
  // Frame selection mirrors Shot_HandleShot's shot sprite update. The Light
  // Blaster (and most unguided projectiles) has flags_primary bit 0 clear, so
  // it takes the *static* (heading-oriented) branch: its sprite set is a
  // 36-frame rotation sheet and the frame is picked from range_scalar_runtime
  // -- the shot's firing bearing in radians -- as
  //   frame = ROUND(frames_per_rotation * bearing / 2pi),
  // the same rotation mapping the ship uses (FrameForHeading). Because a
  // shot's velocity was spawned along that bearing (Math_AddPolarVelocity),
  // the frame is derived here from the velocity's direction.
  //
  // A weapon with flags_primary bit 0 SET takes Shot_HandleShot's *animated*
  // branch instead: NovaWeapon_TickShots steps the shot's frame_cycle_index /
  // anim_elapsed at the weapon's shot_anim_frame_dwell cadence (see
  // weapon.cpp), and here the cycle is wrapped at the shot set's frame count
  // (or held at frame_count-1 for a flags_secondary bit 1 reverse wrap, the
  // last-frame hold Shot_HandleShot applies) before being drawn.
  for (const auto &s : state.active_shots) {
    // The shot's bank slot is a zero-based weapon id; the scenario Weapon()
    // lookup uses the 0x80.. resource-id residue (same convention as
    // NovaWeapon_FirePlayerWeaponBank).
    const Weapon *w = state.scenario.Weapon(static_cast<std::int16_t>(
        static_cast<std::int16_t>(s.weapon_id) + 0x80));
    const SpriteAsset *set =
        (w != nullptr) ? sprite_store_.Spin(
                             platform.renderer(),
                             static_cast<std::uint16_t>(w->sprite_id + 3000))
                       : nullptr;
    if (set && !set->frames.empty()) {
      SpriteDrawOptions opts;
      opts.wrap = false;
      int frame;
      if ((w->flags & 0x0001U) == 0) {
        // Static/heading branch: the frame is the shot's firing bearing.
        const float bearing = std::atan2(s.vel_x, -s.vel_y);
        frame = FrameForHeading(bearing, set->frame_count);
      } else {
        // Time-animated branch: clamp the stepped frame_cycle_index at the
        // set's frame count, holding the last frame for a flags_secondary bit 1
        // reverse-wrap (Shot_HandleShot's frame-count wrap + reverse hold).
        int cycle = s.frame_cycle_index;
        if (cycle >= set->frame_count) {
          cycle =
              ((w->flags_secondary & 0x0002U) != 0) ? set->frame_count - 1 : 0;
        }
        frame = std::clamp(cycle, 0, set->frame_count - 1);
      }
      DrawSprite(renderer,
                 *set,
                 frame,
                 s.pos_x,
                 s.pos_y,
                 camera_x,
                 camera_y,
                 vp.w,
                 vp.h,
                 opts);
    } else {
      float sx = (s.pos_x - camera_x) + static_cast<float>(vp.w) / 2;
      float sy = (s.pos_y - camera_y) + static_cast<float>(vp.h) / 2;
      SDL_SetRenderDrawColor(renderer, 255, 180, 64, SDL_ALPHA_OPAQUE);
      const float radius = 2.0F;
      const SDL_FRect rect{
          sx - radius, sy - radius, radius * 2.0F, radius * 2.0F};
      SDL_RenderFillRect(renderer, &rect);
    }
  }
}

void SpaceflightView::DrawImpactEffects(SdlPlatform &platform,
                                        const GameState &state) {
  const Viewport vp = CurrentViewport(platform);
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  for (const ImpactEffectInstance &instance : state.impact_effect_instances) {
    if (instance.anim_time < 0.0F || instance.delay_timer > 0.0F) {
      continue;
    }
    const ImpactEffect *definition =
        state.scenario.ImpactEffectAt(instance.effect_id);
    if (definition == nullptr) {
      continue;
    }
    const SpriteAsset *set = sprite_store_.Spin(
        platform.renderer(),
        static_cast<std::uint16_t>(400 + definition->sprite_set_id));
    if (set == nullptr || set->frames.empty()) {
      continue;
    }
    const int frame =
        std::clamp(static_cast<int>(std::lround(instance.anim_time)),
                   0,
                   set->frame_count - 1);
    SpriteDrawOptions opts;
    // Ghidra computes distance_intensity (+0xa2) as 32 - anim_time/frame_count
    // for impact sprites. The SDL renderer expresses that as normalized alpha.
    opts.alpha_mod = std::clamp(1.0F - instance.anim_time /
                                           static_cast<float>(set->frame_count),
                                0.0F,
                                1.0F);
    DrawSprite(platform.renderer(),
               *set,
               frame,
               instance.pos_x,
               instance.pos_y,
               camera_x,
               camera_y,
               vp.w,
               vp.h,
               opts);
  }
}

// Ghidra 0x00438c40 (unnamed under-ships beam pass): draw proc of the second
// gameplay sprite-world layer, below shots and ships. Draws only beams whose
// weapon sets flags_secondary 0x2000 (Bible "display the beam underneath
// ships"); all other beams render on the topmost layer instead.
void SpaceflightView::DrawBeamsUnderShips(SdlPlatform &platform,
                                          const GameState &state) {
  for (const BeamHit &beam : state.beam_hit_queue) {
    if (!BeamRecordVisible(state, beam)) {
      continue;
    }
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(beam.weapon_id + 0x80));
    if ((weapon->flags_secondary & 0x2000U) == 0U) {
      continue;
    }
    DrawQueuedBeam(platform, beam_jitter_rng_, state, beam, *weapon);
  }
}

// Ghidra Shot_DrawBeamHitQueueForSurface (0x00438810), visible path: the
// topmost gameplay layer draws every beam that does NOT set flags_secondary
// 0x2000 (those render underneath ships via DrawBeamsUnderShips). The
// original's kinked/flare branches and its twin Shot_DrawBeamQueue
// (0x004386f0) copy saved background pixels back along the previous frame's
// beam line — sprite-world save/restore erase machinery with no SDL
// equivalent, deliberately not reproduced.
// TODO(decomp(0x00438810)) skipped: twin-surface / erase branches.
void SpaceflightView::DrawBeamsOverShips(SdlPlatform &platform,
                                         const GameState &state) {
  for (const BeamHit &beam : state.beam_hit_queue) {
    if (!BeamRecordVisible(state, beam)) {
      continue;
    }
    const Weapon *weapon =
        state.scenario.Weapon(static_cast<std::int16_t>(beam.weapon_id + 0x80));
    if ((weapon->flags_secondary & 0x2000U) != 0U) {
      continue;
    }
    DrawQueuedBeam(platform, beam_jitter_rng_, state, beam, *weapon);
  }
}

// Draws the whole in-flight world, compositing every visible entity in the
// fixed layer order the original's sprite layers use (Ghidra Frame_Spaceflight-
// Loop scope 2 sprite-world present: the background is cleared/filled first,
// then the spiralled sprite layers are drawn in layer order). The precedence,
// bottom to top, is locked as:
//
//   background (space tint + ambient starfield)   -- DrawBackground
//   under-ships beams (flags_secondary 0x2000)     -- DrawBeamsUnderShips
//   stellar bodies (planets / stations)            -- DrawStellarBodies
//   shots / projectiles                           -- DrawShots
//   NPC ships (active ships in the current system) -- DrawNpcShips
//   player ship + engine-glow                     -- (below)
//   over-ships beams (all other beams)             -- DrawBeamsOverShips
//
// That is: the player's hull and glow composite over everything else in the
// scene (front-most), NPC ships and shots pass between the ship and the
// stellar/backdrop layers, and beam weapons render in two of the original's
// sprite-world layers: 0x2000-flagged beams sit on the second layer (above
// the backdrop only, Ghidra 0x00438c40) while every other beam sits on the
// topmost layer above ships and effects (Ghidra Shot_DrawBeamHitQueueFor-
// Surface 0x00438810). Keeping the order
// explicit here (rather than spread across the per-subsystem drawers) makes the
// composed precedence auditable and lets a future layer-table refactor replace
// the fixed sequence wholesale.
void SpaceflightView::Draw(SdlPlatform &platform, const GameState &state) {
  DrawBackground(platform, state); // backmost: tint + ambient stars
  DrawBeamsUnderShips(platform,
                      state);         // flags-0x2000 beams above backdrop only
  DrawStellarBodies(platform, state); // stellar planets / stations
  DrawShots(platform, state);         // projectiles above stellars
  DrawNpcShips(platform, state);      // NPC ships above the backdrop/shots
  DrawImpactEffects(platform, state); // destruction/impact effects over ships
  DrawFadingEffects(platform, state); // directional destruction fragments
  DrawShipTargetReticle(platform, state);   // target brackets over the ships
  DrawTravelTargetReticle(platform, state); // brackets over the travel target
  DrawBeamsOverShips(platform, state);      // topmost layer: normal beams

  // Player ship at the play-area centre, frame selected by heading. Because
  // the camera is centred on the player, drawing at the ship's own world
  // position lands it at the viewport centre (world==camera -> centre).
  if (!ship_.frames.empty() && ship_frames_per_rotation_ > 0) {
    const Viewport vp = CurrentViewport(platform);
    const int frame = ComposeShipFrameIndex(state.player,
                                            ship_sprite_behavior_flags_,
                                            ship_frames_per_rotation_,
                                            ship_row_count_);
    DrawSprite(platform.renderer(),
               ship_,
               frame,
               state.player.pos_x,
               state.player.pos_y,
               state.player.pos_x,
               state.player.pos_y,
               vp.w,
               vp.h);

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
        !glow_.frames.empty()) {
      if (!glow_last_drawn_) {
        NovaLog::Info("[glow] draw ON intensity={:.2f} frames={} size={}x{}",
                      state.player.engine_glow_intensity,
                      glow_.frame_count,
                      glow_.tile_width,
                      glow_.tile_height);
        glow_last_drawn_ = true;
      }
      const int glow_frame =
          std::min(frame, std::max(0, glow_.frame_count - 1));
      SpriteDrawOptions opts;
      opts.alpha_mod = state.player.engine_glow_intensity;
      DrawSprite(platform.renderer(),
                 glow_,
                 glow_frame,
                 state.player.pos_x,
                 state.player.pos_y,
                 state.player.pos_x,
                 state.player.pos_y,
                 vp.w,
                 vp.h,
                 opts);
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

// Ghidra NovaUi_UpdateShipTargetReticle (0x0042ede0). The original positions
// four corner-bracket sprites around the player's primary target ship. Each
// bracket is a cloned sprite from the 16-frame cicn set 10008-10023; the frame
// index is `state_base + corner` where state_base encodes the target's
// disposition:
//   0xc disabled (grey: can't fire -- near-disabled armor < 1/3 max,
//   boarding, travel-to-stellar, or govt no-fire flag;
//   Ship_IsShipDisabled 0x004687b0), 0x8 targeting the player (or a
//   player-targeting chain), 0x0 distress-eligible, 0x4 other.
// The bracket offset is (max(target frame height, target frame width) + 1) / 2
// rounded up plus the decaying reticle pulse; the four sprites are then placed
// at asymmetric screen positions around the target (the 16px corner margin
// falls between the corner art and the ship on the left/top edges only):
//   TL (cx-off-16, cy-off-16)  TR (cx+off,   cy-off-16)
//   BL (cx-off-16, cy+off)     BR (cx+off,   cy+off)
// where off = ceil(max(h,w)/2) + round(pulse). Each bracket frame's anchor is
// its top-left corner (SpriteFrame_CreateFromRect zeroes the anchor pair), so
// the placement point is the frame's top-left and the corner art (a 10-11px
// wedge in the frame's corner quadrant) hugs the target box corner.
// When the cicn set cannot be loaded we fall back to the previous diagnostic
// SDL line brackets (coloured by state) rather than hiding targeting entirely.
const SpriteAsset *SpaceflightView::ShipReticleSet(SdlPlatform &platform) {
  if (!ship_reticle_tried_) {
    ship_reticle_tried_ = true;
    ship_reticle_set_ =
        SpriteAsset::LoadCicnSet(platform.renderer(), 10008, 16);
    if (!ship_reticle_set_) {
      NovaLog::Warn("ship reticle: cicn 10008-10023 unavailable; using debug "
                    "brackets");
    }
  }
  return ship_reticle_set_.get();
}

const SpriteAsset *SpaceflightView::TravelReticleSet(SdlPlatform &platform) {
  if (!travel_reticle_tried_) {
    travel_reticle_tried_ = true;
    travel_reticle_set_ =
        SpriteAsset::LoadCicnSet(platform.renderer(), 10000, 8);
    if (!travel_reticle_set_) {
      NovaLog::Warn("travel reticle: cicn 10000-10007 unavailable");
    }
  }
  return travel_reticle_set_.get();
}

void SpaceflightView::DrawShipTargetReticle(SdlPlatform &platform,
                                            const GameState &state) {
  const std::int16_t slot = state.player.primary_target_ship_slot;
  if (slot <= 0 || !state.SlotInRange(static_cast<std::size_t>(slot))) {
    return; // no (or invalid) primary target: original hides the brackets
  }
  const Ship &target = state.ShipAt(static_cast<std::size_t>(slot));
  if (!target.is_active || NovaAiShip_IsDestroyed(target) ||
      target.current_system_id != state.player.current_system_id) {
    return;
  }
  const Viewport vp = CurrentViewport(platform);
  const float pulse = std::max(0.0F, state.ship_reticle_pulse);
  // The brackets are placed in WORLD space around the target; DrawSprite's
  // single camera transform then lands them on screen at the target. This
  // mirrors the original, which positions the bracket sprites at
  // round(target - player) + g_viewport_center_x/y (Ghidra 0x0042ede0 reads
  // g_viewport_center_x/y -- shown in the decompile as the overlapping
  // _236_2_/_238_2_ sub-symbols -- exactly like every other gameplay sprite).
  // cx/cy are the target's SCREEN centre, used by the diagnostic fallback
  // below which blits in screen space directly.
  const float wx = target.pos_x;
  const float wy = target.pos_y;
  const float cx = (wx - state.player.pos_x) + static_cast<float>(vp.w) / 2.0F;
  const float cy = (wy - state.player.pos_y) + static_cast<float>(vp.h) / 2.0F;

  // Bracket frame base by target state (mirrors the reticle's sVar4 branch).
  int frame_base = 4; // other
  if (NovaAiShip_IsDisabled(state, target)) {
    frame_base = 0xc;
  } else {
    if (target.squad_leader_ship_slot == 0 &&
        target.target_stellar_object_id == -1) {
      frame_base = 8; // directly targeting the player
    } else {
      frame_base =
          NovaTargeting_IsShipEligibleForDistressCall(state, target) ? 0 : 4;
    }
    // The original re-checks the chain AFTER the state branches and overrides
    // 0/4: targeting a ship that itself targets the player reads as 8.
    if (target.squad_leader_ship_slot > 0 &&
        state.SlotInRange(
            static_cast<std::size_t>(target.squad_leader_ship_slot)) &&
        state.ShipAt(static_cast<std::size_t>(target.squad_leader_ship_slot))
                .squad_leader_ship_slot == 0) {
      frame_base = 8;
    }
  }

  // Bracket offset: ceil(max(target frame height, width)/2) + the decaying
  // pulse. Sprite_GetFrameVerticalHalfSpan / Sprite_GetShotHalfSpan return the
  // target's full frame height/width, so `full` is max(h, w), then the game
  // halves it rounding up; the pulse term is rounded to whole pixels exactly
  // as the original does. The fallback uses the sheet's native tile size.
  float full = 32.0F; // default Sprite_Get*HalfSpan for a missing sprite
  if (const NpcShipSprite *sprite = ShipClassSprite(
          platform, static_cast<std::int16_t>(target.ship_class_id + 0x80));
      sprite != nullptr && !sprite->base.frames.empty()) {
    full = std::max(static_cast<float>(sprite->base.tile_width),
                    static_cast<float>(sprite->base.tile_height));
  }
  const float off = std::ceil(full * 0.5F) + std::round(pulse);

  // The real corner brackets, when the cicn set is available. The frames'
  // anchors are their top-left corners (LoadCicnSet mirrors
  // SpriteFrame_CreateFromRect's (0,0) anchor), so each placement point is the
  // bracket frame's top-left -- exactly how the original's
  // Sprite_SetPositionFromCurrentFrameAnchor places them (no half-span
  // compensation, unlike the ship/stellar updaters). The corner art is a
  // 10-11px wedge in the frame's corner quadrant, so the wedge's diagonal
  // hugs the target box corner with a small gap.
  if (const SpriteAsset *ret = ShipReticleSet(platform)) {
    // Corner indices 0..3 map to TL, TR, BR, BL (matching the order in which
    // the original positions its four bracket sprites). The offsets (off and
    // the 16px corner margin) are in world pixels, so they are added to the
    // target's WORLD position here rather than to the screen centre -- the
    // DrawSprite call below applies the camera transform exactly once.
    const float pos[4][2] = {{wx - off - 16.0F, wy - off - 16.0F},
                             {wx + off, wy - off - 16.0F},
                             {wx + off, wy + off},
                             {wx - off - 16.0F, wy + off}};
    for (int corner = 0; corner < 4; ++corner) {
      DrawSprite(platform.renderer(),
                 *ret,
                 frame_base + corner,
                 pos[corner][0],
                 pos[corner][1],
                 state.player.pos_x,
                 state.player.pos_y,
                 vp.w,
                 vp.h);
    }
    return;
  }

  // Diagnostic fallback: L-shaped corner brackets coloured to approximate the
  // real cicn art per state (10008-10023 palettes: 0xc grey, 0x8 green,
  // 0x0 red/orange, 0x4 black/yellow).
  const SDL_Color color = [&]() -> SDL_Color {
    switch (frame_base) {
    case 0xc:
      return SDL_Color{150, 150, 150, SDL_ALPHA_OPAQUE}; // disabled
    case 0x8:
      return SDL_Color{80, 255, 120, SDL_ALPHA_OPAQUE}; // engaged w/ player
    case 0x0:
      return SDL_Color{255, 120, 40, SDL_ALPHA_OPAQUE}; // distress-eligible
    default:
      return SDL_Color{255, 255, 100, SDL_ALPHA_OPAQUE}; // other
    }
  }();
  const float left = cx - off;
  const float right = cx + off;
  const float top = cy - off;
  const float bottom = cy + off;
  constexpr float kArm = 14.0F; // bracket arm length (provisional)
  SDL_SetRenderDrawColor(
      platform.renderer(), color.r, color.g, color.b, color.a);
  const auto corner = [&](float x, float y, float dx, float dy) {
    SDL_FRect h{std::min(x, x + dx), y, std::abs(dx), 3.0F};
    SDL_FRect v{x, std::min(y, y + dy), 3.0F, std::abs(dy)};
    SDL_RenderFillRect(platform.renderer(), &h);
    SDL_RenderFillRect(platform.renderer(), &v);
  };
  corner(left, top, kArm, kArm);                   // top-left
  corner(right - kArm, top, kArm, kArm);           // top-right
  corner(left, bottom - kArm, kArm, kArm);         // bottom-left
  corner(right - kArm, bottom - kArm, kArm, kArm); // bottom-right
}

// Ghidra NovaUi_UpdateTravelTargetReticle (0x0042eac0). While a travel
// destination stellar is selected (state.travel.selected_stellar_id, the
// original's ai_secondary_target_slot >= 0) the four corner brackets are drawn
// around that stellar at its screen position, using the 8-frame cicn set
// 10000-10007 (frame = base + corner; base is 0 or 4 by the destination's
// hazard marker). The offset is
// ceil(max(frame height, frame width)/2) + the decaying travel pulse, sized by
// the destination stellar's sprite set; the corners use the same asymmetric
// placement as the ship reticle. There is no SDL-line fallback: a missing cicn
// set just hides the reticle (the stellar remains highlighted by selection).
void SpaceflightView::DrawTravelTargetReticle(SdlPlatform &platform,
                                              const GameState &state) {
  const std::int16_t stellar_id = state.travel.selected_stellar_id;
  if (stellar_id < 0x80) {
    return; // no destination selected
  }
  // The original draws the brackets only while travel_transfer_mode == 2
  // (stellar navigation): a plotted jump (mode 3) or an engaged sequence
  // hides them regardless of the selection (0x0042eac0's early-out).
  if (state.travel.engaging || state.travel.hyperspace_mode ||
      state.player.travel_transfer_mode == 3) {
    return;
  }
  const auto *st = state.scenario.Stellar(stellar_id);
  if (!st || !st->is_available ||
      st->system_id != state.player.current_system_id) {
    return;
  }
  const SpriteAsset *ret = TravelReticleSet(platform);
  if (!ret) {
    return;
  }
  const Viewport vp = CurrentViewport(platform);
  const float pulse = std::max(0.0F, state.travel_reticle_pulse);
  // Same world-space placement as the ship reticle: bracket corners are offset
  // from the stellar's WORLD position and DrawSprite applies the single camera
  // transform (original: round(stellar.map - player) + g_viewport_center).
  const float wx = static_cast<float>(st->pos_x);
  const float wy = static_cast<float>(st->pos_y);

  // Bracket offset sized by the destination stellar's spin sprite set; the
  // pulse term is rounded to whole pixels (original rounds it too).
  float full = 32.0F; // default Sprite_Get*HalfSpan fallback (0x20)
  if (const auto *spin =
          sprite_store_.Spin(platform.renderer(),
                             static_cast<std::uint16_t>(st->link_a_id + 1000));
      spin && !spin->frames.empty()) {
    full = std::max(static_cast<float>(spin->tile_width),
                    static_cast<float>(spin->tile_height));
  }
  const float off = std::ceil(full * 0.5F) + std::round(pulse);

  // Frame base by the destination's hazard marker (Ghidra StellarDef
  // hazard_marker +0x46 in NovaUi_UpdateTravelTargetReticle 0x0042eac0:
  // marker clear -> frames 0..3, set -> frames 4..7 of cicn 10000+).
  const int frame_base = st->hazard_marker ? 4 : 0;
  const float pos[4][2] = {{wx - off - 16.0F, wy - off - 16.0F},
                           {wx + off, wy - off - 16.0F},
                           {wx + off, wy + off},
                           {wx - off - 16.0F, wy + off}};
  for (int corner = 0; corner < 4; ++corner) {
    DrawSprite(platform.renderer(),
               *ret,
               frame_base + corner,
               pos[corner][0],
               pos[corner][1],
               state.player.pos_x,
               state.player.pos_y,
               vp.w,
               vp.h);
  }
}

std::int16_t SpaceflightView::PickShipAt(SdlPlatform &platform,
                                         const GameState &state,
                                         float rx,
                                         float ry) {
  const Viewport vp = CurrentViewport(platform);
  const float wx = rx - static_cast<float>(vp.w) / 2.0F + state.player.pos_x;
  const float wy = ry - static_cast<float>(vp.h) / 2.0F + state.player.pos_y;
  std::int16_t best = -1;
  float best_dist_sq = 0.0F;
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || NovaAiShip_IsDestroyed(ship) ||
        ship.current_system_id != state.player.current_system_id) {
      continue;
    }
    const NpcShipSprite *sprite = ShipClassSprite(
        platform, static_cast<std::int16_t>(ship.ship_class_id + 0x80));
    if (sprite == nullptr || sprite->base.frames.empty()) {
      continue;
    }
    const float half_span =
        std::max(sprite->base.tile_width, sprite->base.tile_height) * 0.5F;
    const float dx = ship.pos_x - wx;
    const float dy = ship.pos_y - wy;
    const float dist_sq = dx * dx + dy * dy;
    if (dist_sq > half_span * half_span) {
      continue;
    }
    if (best == -1 || dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = static_cast<std::int16_t>(slot);
    }
  }
  return best;
}

std::int16_t SpaceflightView::PickStellarAt(SdlPlatform &platform,
                                            const GameState &state,
                                            float rx,
                                            float ry) {
  const auto *sys = state.scenario.System(
      static_cast<std::int16_t>(state.player.current_system_id + 0x80));
  if (!sys) {
    return -1;
  }
  const Viewport vp = CurrentViewport(platform);
  const float wx = rx - static_cast<float>(vp.w) / 2.0F + state.player.pos_x;
  const float wy = ry - static_cast<float>(vp.h) / 2.0F + state.player.pos_y;
  std::int16_t best = -1;
  float best_dist_sq = 0.0F;
  for (const auto nav : sys->nav_defs) {
    if (nav < 0x80) {
      continue;
    }
    const auto *st = state.scenario.Stellar(nav);
    if (!st || !st->is_available ||
        st->system_id != state.player.current_system_id) {
      continue;
    }
    // The original hit-tests the live ambient sprite (sprite_handle_active);
    // a stellar with no loaded spin set has no sprite to click.
    const SpriteAsset *set = sprite_store_.Spin(
        platform.renderer(), static_cast<std::uint16_t>(st->link_a_id + 1000));
    if (set == nullptr || set->frames.empty()) {
      continue;
    }
    float half_w = static_cast<float>(set->tile_width) * 0.5F;
    float half_h = static_cast<float>(set->tile_height) * 0.5F;
    if (set->tile_height < 0x30) {
      // Rect_Inset(&rect, -0x10, -0x10): small sprites get a 16px grab halo.
      half_w += 16.0F;
      half_h += 16.0F;
    }
    const float dx = static_cast<float>(st->pos_x) - wx;
    const float dy = static_cast<float>(st->pos_y) - wy;
    if (std::abs(dx) > half_w || std::abs(dy) > half_h) {
      continue;
    }
    const float dist_sq = dx * dx + dy * dy;
    if (best == -1 || dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best = nav;
    }
  }
  return best;
}

bool SpaceflightView::ClickInPlayerSprite(SdlPlatform &platform,
                                          const GameState &state,
                                          float rx,
                                          float ry) {
  if (ship_.frames.empty()) {
    return false;
  }
  const Viewport vp = CurrentViewport(platform);
  const float wx = rx - static_cast<float>(vp.w) / 2.0F + state.player.pos_x;
  const float wy = ry - static_cast<float>(vp.h) / 2.0F + state.player.pos_y;
  float half_w = static_cast<float>(ship_.tile_width) * 0.5F;
  float half_h = static_cast<float>(ship_.tile_height) * 0.5F;
  if (ship_.tile_height < 0x30) {
    half_w += 16.0F;
    half_h += 16.0F;
  }
  const float dx = state.player.pos_x - wx;
  const float dy = state.player.pos_y - wy;
  return std::abs(dx) <= half_w && std::abs(dy) <= half_h;
}

void SpaceflightView::DrawGameFrame(SdlPlatform &platform,
                                    const GameState &state,
                                    HudRenderer &hud) {
  // The free-flight world extends: draw 1:1 across the whole (possibly larger)
  // window with no centre-clipping. Modal windows re-assert their own
  // presentation after this, so set the fullscreen viewport here every frame.
  platform.SetFullscreenPlayfield();
  Draw(platform, state);
  // HUD overlays the extending world at fixed, unscaled size (the project's
  // resolution policy: more window = more system shown, NOT a bigger HUD).
  hud.Draw(platform, state);
  // Hyperspace fire flash: a full-screen white frame at the jump moment (the
  // original's centered effect 0x32 queued at engage, the 'boom' flash).
  // Drawn topmost so it also whites out the HUD, then fades over the next few
  // frames as the loop decays screen_flash_intensity.
  if (state.screen_flash_intensity > 0.0F) {
    SDL_Renderer *const renderer = platform.renderer();
    const std::uint8_t a = static_cast<std::uint8_t>(
        std::clamp(state.screen_flash_intensity, 0.0F, 1.0F) * 255.0F);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, a);
    SDL_RenderFillRect(renderer, nullptr);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  }
}

} // namespace game
