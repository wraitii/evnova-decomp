#include "spaceflight_view.hpp"

#include "../brgr_archive.hpp"
#include "../log.hpp"
#include "../rle_sprite_sheet.hpp"
#include "../sdl_platform.hpp"
#include "asteroid.hpp"
#include "compatibility.hpp"
#include "freeflight_objects.hpp"
#include "game_state.hpp"
#include "gameplay_interface.hpp"
#include "hud_renderer.hpp"
#include "impact_effects.hpp"
#include "nova_math.hpp"
#include "nova_random.hpp"
#include "ship_ai.hpp"
#include "ship_visual.hpp"
#include "targeting.hpp"
#include "travel.hpp"
#include "weapon.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <tuple>

namespace game {
namespace {

// Base logical play area. The live flight viewport is the window minus the
// top-right cockpit/HUD strip (kGameplayHudStripWidth, the original's
// DAT_0088c020), full height; these floors just keep it sane before the window
// exists.
constexpr int kViewportWidth = 640;
constexpr int kViewportHeight = 400;

int HypergateTransitionFrame(const Stellar &stellar, int frame_count) {
  int transition = stellar.custom_picture_or_gate_transition_frame;
  if (transition < 1 || transition >= frame_count - 1) {
    transition = frame_count / 2;
  }
  return transition;
}

bool IsHypergateAnimationEngaged(const GameState &state,
                                 std::int16_t stellar_id,
                                 const Stellar &stellar,
                                 int current_frame,
                                 int frame_count,
                                 int frame_width) {
  // Ghidra 0x004159c0 Ship_IsShipInAiState0x15 and 0x00415ad0
  // Stellar_IsShipHeadingToStellarInAiState0x01Or0x14 run inline here.
  if (state.travel.selected_stellar_id == stellar_id &&
      state.travel.engage_timer > 0) {
    return true;
  }
  const int transition = HypergateTransitionFrame(stellar, frame_count);
  // 0x0042cd10 (0x0042cf..) sizes the range from Sprite_GetFrameFullWidth
  // (0x00462390) * 2 and expands it by 1.1 once open (DAT_005753a8).
  float range = static_cast<float>(frame_width * 2);
  if (current_frame >= transition) {
    range *= 1.1F; // Ghidra DAT_005753a8.
  }
  for (std::size_t slot = 1; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (!ship.is_active || NovaAiShip_IsDisabled(state, ship)) {
      continue;
    }
    if (ship.ai_state_code == 0x15 &&
        ship.ai_secondary_target_slot == stellar_id) {
      return true;
    }
    if ((ship.ai_state_code == 1 || ship.ai_state_code == 0x14) &&
        ship.ai_secondary_target_slot == stellar_id &&
        std::abs(ship.pos_x - static_cast<float>(stellar.pos_x)) < range &&
        std::abs(ship.pos_y - static_cast<float>(stellar.pos_y)) < range) {
      return true;
    }
  }
  return false;
}

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

// @port 0x0046a870 70% rendering
// Ghidra 0x0046a870 NovaUi_RedrawGameplayViewportAndRadar. The synchronous
// viewport + radar redraw is expressed structurally: SpaceflightView::Draw
// rebuilds the world each frame and HudRenderer::DrawRadarPanel redraws the
// radar; the SDL port has no saved-backdrop offscreen surface.
// The flight world's camera viewport, in window points: the render owner minus
// the right-hand cockpit strip. The original's play area is
// `[RenderOwner.left, RenderOwner.right - DAT_0088c020]` (see
// NovaUi_RedrawGameplayViewportAndRadar 0x0046a870), so the ship-centred camera
// (g_viewport_center_x/y, set by Ship_InitializeMainInterface 0x004ac380) sits
// at half this width. Using the full window here would push the world and the
// asteroid field kGameplayHudStripWidth/2 too far right.
[[nodiscard]] Viewport CurrentViewport(const SdlPlatform &platform) {
  const auto sz = platform.logical_playfield_size();
  const int window_w = std::max(kViewportWidth, static_cast<int>(sz.x));
  const int w = window_w - kGameplayHudStripWidth;
  const int h = std::max(kViewportHeight, static_cast<int>(sz.y));
  return {w, h};
}

// Ghidra Ship_UpdateVisualState (0x00428340) frame composition: the displayed
// index is row * frames_per_rotation + heading_frame. The row comes from
// ComposeShipBaseRow (banking / fold / carry / combat-sequence flags) and is
// clamped to the loaded rows so a sheet shorter than the flags imply falls
// back safely.
[[nodiscard]] int ComposeShipFrameIndex(const Ship &ship,
                                        int row,
                                        int frames_per_rotation,
                                        int row_count) {
  const int clamped_row = std::clamp(row, 0, std::max(1, row_count) - 1);
  return clamped_row * frames_per_rotation +
         FrameForHeading(ship.heading, frames_per_rotation);
}

// ---- beam rendering (Ghidra SWBeams.c family) -----------------------------

[[nodiscard]] std::tuple<std::uint8_t, std::uint8_t, std::uint8_t>
SplitRgb(std::uint32_t packed) {
  return {static_cast<std::uint8_t>(packed >> 16 & 0xffU),
          static_cast<std::uint8_t>(packed >> 8 & 0xffU),
          static_cast<std::uint8_t>(packed & 0xffU)};
}

// @port 0x00479590,0x004797b0 60% rendering
// TODO(decomp): SWBeams_SetupAndClipSegment scope not reproduced.
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

// @port 0x00479fe0 85% rendering
// Divergence: 15-bit blending -> SDL.
// TODO(decomp): bounds pre-cull is skipped.
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

// @port 0x0047a410 95% rendering,rng
// TODO(decomp): the twin-surface DrawBeamWithFlare variant (0x0047AFD0) and the
// erase pass remain separate/0%. RNG stream divergence documented at the call
// site. Ghidra SWBeams_DrawThickFadingBeam (0x0047a410): lightning-beam
// plotter. Splits the muzzle->target span into round(max(|dx|,|dy|) * density *
// 0.01) jittered segments (the original multiplies by the double DAT_00575888 =
// 0.01, disasm 0x0047a587). Jitter is uniform on [-amplitude, amplitude]
// applied to every segment except the single final one, which lands on the
// exact target. Each segment draws parallel lines at perpendicular offsets
// 0..width-1 (mirrored past offset 0) at alpha base - j*0x20/(width+1); only
// the final segment fades, over 0x20 px.
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
  const int dx = xb - xa;
  const int dy = yb - ya;
  const int reach = std::max(std::abs(dx), std::abs(dy));
  // The original converts the product with the x87 FIST + residual/sign
  // correction idiom (ADD 0x7fffffff; SBB), which nets truncation toward zero,
  // not lround/nearbyint. The product is non-negative, so static_cast is floor.
  const int steps = static_cast<int>(static_cast<double>(reach) *
                                     static_cast<double>(density) * 0.01);
  if (steps < 2) {
    // The original skips the plot entirely when the step count truncates to 1.
    return;
  }
  const float step_x = static_cast<float>(dx) / static_cast<float>(steps);
  const float step_y = static_cast<float>(dy) / static_cast<float>(steps);
  std::uniform_int_distribution<int> jitter{-amplitude, amplitude};

  const bool horizontal = std::abs(dx) >= std::abs(dy);
  const auto [cr, cg, cb] = SplitRgb(color);
  const int alpha_step = 0x20 / (width + 1);

  float fx = static_cast<float>(xa);
  float fy = static_cast<float>(ya);
  int prev_x = xa;
  int prev_y = ya;
  for (int idx = 0; idx < steps - 1; ++idx) {
    fx += step_x;
    fy += step_y;
    int cur_x;
    int cur_y;
    if (idx < steps - 2) {
      cur_x = static_cast<int>(std::nearbyint(fx)) + jitter(rng);
      cur_y = static_cast<int>(std::nearbyint(fy)) + jitter(rng);
    } else {
      cur_x = xb;
      cur_y = yb;
    }
    const int end_fade = idx == steps - 2 ? 0x20 : 0;
    for (int j = 0; j < width; ++j) {
      const int alpha = std::max(1, base_alpha - j * alpha_step);
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
  const int width = weapon.beam_width_or_animation_frame_delay;
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

// Loads one optional sh\x8an sprite layer (engine glow / running lights /
// weapon effects) into `asset`. The original's
// Sprite_CreateFromSpriteSheetResources call gives every role the same
// frames_per_rotation * base_set_count frame count as the hull, so the layer
// shares the base heading rotation grid. A non-positive image id means the
// class has no such layer; a load failure is logged and non-fatal.
void LoadShipVisualLayer(SDL_Renderer *renderer,
                         std::int16_t image_id,
                         const char *role,
                         const std::string &display_name,
                         int base_frame_count,
                         SpriteAsset &asset,
                         bool &present) {
  present = image_id > 0;
  if (!present) {
    return;
  }
  if (auto layer = SpriteAsset::LoadSheet(renderer,
                                          static_cast<std::uint16_t>(image_id));
      layer) {
    asset = std::move(*layer);
    // The original's Sprite_CreateFromSpriteSheetResources call gives every
    // role the base rotation grid's frame count; keep that but never exceed
    // the frames actually decoded (a short/malformed sheet must not make the
    // renderer index past its frame vector).
    asset.frame_count =
        std::min(base_frame_count, static_cast<int>(asset.frames.size()));
    NovaLog::Info("ship {} layer loaded for '{}': {}x{} x{} frames",
                  role,
                  display_name,
                  asset.tile_width,
                  asset.tile_height,
                  asset.frame_count);
  } else {
    present = false;
    NovaLog::Warn("ship sprite: no usable rl.x91D {} sheet {} for class '{}'; "
                  "layer skipped",
                  role,
                  image_id,
                  display_name);
  }
}

// Draws one additive ship effect layer (running lights / weapon effects). The
// original stores round(intensity) in the Sprite's tint channels with
// brightness_level 32; SDL has no per-channel tint here, so the brightness
// maps to a multiplicative alpha (level/32), matching the existing engine-glow
// approximation. `visible_threshold` mirrors the original's hide test (the
// light layer hides at <= 1.0, the weapon layer at <= 0).
void DrawShipEffectLayer(SDL_Renderer *renderer,
                         const SpriteAsset &layer,
                         int frame,
                         float world_x,
                         float world_y,
                         float camera_x,
                         float camera_y,
                         int viewport_w,
                         int viewport_h,
                         float intensity,
                         float visible_threshold,
                         int fog_murk) {
  if (layer.frames.empty() || intensity <= visible_threshold) {
    return;
  }
  SpriteDrawOptions opts;
  opts.alpha_mod = std::clamp(intensity / 32.0F, 0.0F, 1.0F);
  opts.fog_murk = fog_murk;
  // The original's light/weapon layers use the tinted draw proc at brightness
  // 0x20, i.e. dst + src*intensity/0x20 (additive).
  opts.additive = true;
  DrawSprite(renderer,
             layer,
             frame,
             world_x,
             world_y,
             camera_x,
             camera_y,
             viewport_w,
             viewport_h,
             opts);
}

} // namespace

void NovaStellar_AdvanceAnimationFrame(GameState &state,
                                       const Stellar &stellar,
                                       int frame_count,
                                       bool engaged,
                                       float elapsed_30hz_ticks,
                                       StellarAnimationState &animation) {
  if (frame_count < 2) {
    animation.current_frame = 0;
    return;
  }
  animation.frame_accumulator += elapsed_30hz_ticks;
  const auto random_frame = [&](int count) {
    return std::uniform_int_distribution<int>{0, count - 1}(state.rng);
  };

  if ((stellar.availability_flags & Stellar::kHypergate) == 0) {
    const int dwell =
        animation.current_frame == 0 && stellar.animation_frame_multiplier > 1
            ? stellar.animation_dwell_time * stellar.animation_frame_multiplier
            : stellar.animation_dwell_time;
    if (animation.frame_accumulator < static_cast<float>(dwell)) {
      return;
    }
    animation.frame_accumulator = 0.0F;
    if ((stellar.availability_flags & Stellar::kAnimationReturnToFirstFrame) ==
        0) {
      animation.current_frame = animation.previous_frame;
      if ((stellar.availability_flags & Stellar::kAnimationChooseRandomFrame) ==
          0) {
        animation.previous_frame = (animation.previous_frame + 1) % frame_count;
      } else {
        do {
          animation.previous_frame = random_frame(frame_count);
        } while (animation.previous_frame == animation.current_frame);
      }
    } else if (animation.current_frame == 0) {
      animation.current_frame = animation.previous_frame;
      if ((stellar.availability_flags & Stellar::kAnimationChooseRandomFrame) ==
          0) {
        animation.previous_frame = (animation.previous_frame + 1) % frame_count;
        if (animation.previous_frame == 0) {
          ++animation.previous_frame;
        }
      } else {
        do {
          do {
            animation.previous_frame = random_frame(frame_count);
          } while (animation.previous_frame == 0);
        } while (animation.previous_frame == animation.current_frame);
      }
    } else {
      animation.current_frame = 0;
    }
    return;
  }

  const int transition = HypergateTransitionFrame(stellar, frame_count);
  const int dwell =
      engaged && animation.current_frame == transition &&
              stellar.animation_frame_multiplier > 1
          ? stellar.animation_dwell_time * stellar.animation_frame_multiplier
          : stellar.animation_dwell_time;
  if (animation.frame_accumulator < static_cast<float>(dwell)) {
    return;
  }
  animation.frame_accumulator = 0.0F;
  if (!engaged) {
    if (animation.current_frame < transition) {
      if (animation.current_frame > 0) {
        --animation.current_frame;
      }
    } else if ((stellar.availability_flags &
                Stellar::kAnimationChooseRandomFrame) == 0) {
      if (animation.current_frame < frame_count - 1) {
        ++animation.current_frame;
      } else {
        animation.current_frame = transition - 1;
      }
    } else {
      animation.current_frame = transition - 1;
    }
    return;
  }

  if (animation.current_frame < transition) {
    ++animation.current_frame;
    animation.previous_frame = animation.current_frame;
  } else if ((stellar.availability_flags &
              Stellar::kAnimationReturnToFirstFrame) == 0) {
    animation.current_frame = animation.previous_frame;
    if ((stellar.availability_flags & Stellar::kAnimationChooseRandomFrame) ==
        0) {
      animation.previous_frame = (animation.previous_frame + 1) % frame_count;
      animation.previous_frame = std::max(animation.previous_frame, transition);
    } else {
      do {
        animation.previous_frame =
            transition + random_frame(frame_count - transition);
      } while (animation.previous_frame == animation.current_frame);
    }
  } else if (animation.current_frame == transition) {
    animation.current_frame = animation.previous_frame;
    if ((stellar.availability_flags & Stellar::kAnimationChooseRandomFrame) ==
        0) {
      animation.previous_frame = (animation.previous_frame + 1) % frame_count;
      if (animation.previous_frame < transition) {
        animation.previous_frame = transition + 1;
      }
    } else {
      do {
        do {
          animation.previous_frame =
              transition + random_frame(frame_count - transition);
        } while (animation.previous_frame == transition);
      } while (animation.previous_frame == animation.current_frame);
    }
  } else {
    animation.current_frame = transition;
  }
}

ShipEmergencePresentation NovaShip_EmergencePresentation(const Ship &ship) {
  if (ship.ai_state_code != 0x15) {
    return {};
  }
  constexpr float kFadeTicks = 16.0F; // DAT_00575370
  if (ship.ai_maneuver_timer_ms > kFadeTicks) {
    return {.visible = false, .hull_alpha = 0.0F, .white_mix = 1.0F};
  }
  const float ticks = std::clamp(ship.ai_maneuver_timer_ms, 0.0F, kFadeTicks);
  // Original Sprite brightness_level is transparency (0 opaque, 32 clear).
  const float transparency = (ticks * 2.0F) / 32.0F;
  // distance_brightness = min(brightness_level * 2, 32), space color white.
  return {.visible = true,
          .hull_alpha = 1.0F - transparency,
          .white_mix = std::min(ticks / 8.0F, 1.0F)};
}

bool SpaceflightView::EnsureShipSprite(SdlPlatform &platform,
                                       GameState &state) {
  // The player uses the same per-class sprite cache as the NPCs; a class
  // change (shipyard purchase / capture) simply loads the new class entry.
  const std::int16_t player_class =
      static_cast<std::int16_t>(state.player.ship_class_id + 0x80);
  const ShipSpriteSet *set = ShipSprites(platform, player_class);
  return set != nullptr && !set->base.frames.empty();
}

// Loads (and caches) a ship class's sh\x8an sprite layers for a class resource
// id. Covers the base hull plus the alt, engine-glow, running-lights,
// weapon-effects and shield overlay sheets the class descriptor names; the
// player and NPCs share the cache. Returns a pointer to the cached entry, or
// null when the class/base sheet cannot be loaded.
const SpaceflightView::ShipSpriteSet *
SpaceflightView::ShipSprites(SdlPlatform &platform,
                             std::int16_t ship_class_id) {
  const auto cached = ship_sprites_.find(ship_class_id);
  if (cached != ship_sprites_.end()) {
    return cached->second.base.frames.empty() ? nullptr : &cached->second;
  }

  ShipSpriteSet entry;
  const auto visual_data = NovaResource_Load(
      kShipVisualResourceType, static_cast<std::uint16_t>(ship_class_id));
  const auto visual =
      visual_data ? DecodeShipVisualDescriptor(*visual_data) : std::nullopt;
  if (!visual) {
    // Cache a failed entry so a missing class is not retried every frame.
    ship_sprites_[ship_class_id] = std::move(entry);
    return nullptr;
  }
  auto base =
      SpriteAsset::LoadSheet(platform.renderer(), visual->base_image_id);
  if (!base) {
    ship_sprites_[ship_class_id] = std::move(entry);
    return nullptr;
  }
  entry.base = std::move(*base);
  entry.frames_per_rotation = visual->frames_per_rotation;
  entry.row_count =
      std::max(1, entry.base.frame_count / entry.frames_per_rotation);
  entry.alt_set_count =
      static_cast<int>(std::max<std::int16_t>(0, visual->alt_set_count));
  // Overlay layers, all sharing the base's rotation grid. A non-positive image
  // id, or a sheet that fails to load, just means the class lacks that layer
  // (not fatal). The alt sheet's frame count is AltSetCount * FramesPer; the
  // rest are sized to the base grid.
  const std::string class_label =
      "class " + std::to_string(static_cast<unsigned>(ship_class_id));
  if (visual->alt_image_id > 0 && entry.alt_set_count > 0) {
    LoadShipVisualLayer(platform.renderer(),
                        visual->alt_image_id,
                        "alt sprite",
                        class_label,
                        entry.alt_set_count * entry.frames_per_rotation,
                        entry.alt,
                        entry.has_alt);
  }
  LoadShipVisualLayer(platform.renderer(),
                      visual->engine_glow_image_id,
                      "engine glow",
                      class_label,
                      entry.base.frame_count,
                      entry.glow,
                      entry.has_glow);
  LoadShipVisualLayer(platform.renderer(),
                      visual->light_image_id,
                      "running lights",
                      class_label,
                      entry.base.frame_count,
                      entry.light,
                      entry.has_light);
  LoadShipVisualLayer(platform.renderer(),
                      visual->weapon_image_id,
                      "weapon effects",
                      class_label,
                      entry.base.frame_count,
                      entry.weapon,
                      entry.has_weapon);
  LoadShipVisualLayer(platform.renderer(),
                      visual->shield_image_id,
                      "shield bubble",
                      class_label,
                      entry.base.frame_count,
                      entry.shield,
                      entry.has_shield);
  auto [it, inserted] = ship_sprites_.emplace(ship_class_id, std::move(entry));
  (void)inserted;
  NovaLog::Info("ship sprite loaded for class {:#x}: {}x{} x{} frames ({} "
                "set(s)){}{}{}{}{}",
                static_cast<unsigned>(ship_class_id),
                it->second.base.tile_width,
                it->second.base.tile_height,
                it->second.base.frame_count,
                it->second.row_count,
                it->second.has_alt ? " + alt" : "",
                it->second.has_glow ? " + engine glow" : "",
                it->second.has_light ? " + running lights" : "",
                it->second.has_weapon ? " + weapon effects" : "",
                it->second.has_shield ? " + shield" : "");
  return &it->second;
}

// Draws one ship's full composite at its world position: hull -> engine glow ->
// running lights -> weapon effects -> alt overlay -> emergence white. The
// per-ship layer order mirrors the original's six-sprite container append
// order (base 0x0, glow 0x4, light 0x8, weapon 0xc, alt 0x10, shield 0x14), so
// the alt sheet composites over the hull and its additive effect layers. The
// shield bubble is loaded for readiness but its draw stays deferred.
void SpaceflightView::DrawShipSprite(SdlPlatform &platform,
                                     const GameState &state,
                                     const Ship &ship,
                                     const ShipSpriteSet &sprite,
                                     int viewport_w,
                                     int viewport_h,
                                     float camera_x,
                                     float camera_y) {
  const ShipClass *cls =
      state.scenario.Ship(static_cast<std::int16_t>(ship.ship_class_id + 0x80));
  if (cls == nullptr || sprite.base.frames.empty() ||
      sprite.frames_per_rotation <= 0) {
    return;
  }
  const ShipEmergencePresentation emergence =
      NovaShip_EmergencePresentation(ship);
  if (!emergence.visible) {
    return;
  }
  const int frame = ComposeShipFrameIndex(ship,
                                          ComposeShipBaseRow(state, ship, *cls),
                                          sprite.frames_per_rotation,
                                          sprite.row_count);
  // All colored layers (glow/lights/weapon/alt) fade out as the white
  // emergence silhouette takes over; the white pass is drawn last so it hides
  // the colored layers instead of the reverse.
  const float layer_alpha = emergence.hull_alpha * (1.0F - emergence.white_mix);

  // Cloak render slice (Ship_UpdateVisualState 0x00428340 0x0042b0b2..). A
  // fully cloaked hull the player cannot reveal has hull/alt skipped (hull
  // brightness 0x40); a partially cloaked hull is an additive ghost whose
  // composite layer rects all take the same per-frame jitter. The
  // glow/light/shield layers are hidden outright, but the weapon layer still
  // draws under the shared 40 - progress cap.
  const NovaShipTintColor tint = NovaShip_ResolveTintColor(state, ship);
  const ShipCloakPresentation cloak =
      NovaShip_CloakPresentation(state, ship, tint);
  const float hull_x = ship.pos_x + cloak.jitter_x;
  const float hull_y = ship.pos_y + cloak.jitter_y;

  SpriteDrawOptions hull_opts;
  hull_opts.alpha_mod = layer_alpha * cloak.hull_alpha;
  // Base-hull tint from Ship_ResolveShipTintColor (0x0046e470): the player uses
  // the global paint, NPCs their personality/faction color. See
  // SpriteDrawOptions.tint_rgb5 for the SDL approximation.
  if (cloak.additive) {
    // Partial cloak: brightness 0x20 collapses BlitPixel_TintRgb15Span to the
    // additive `dst + src*tint/32`; hull_tint already carries
    // resolved-progress.
    hull_opts.additive = true;
    hull_opts.tint_rgb5 = cloak.hull_tint;
  } else if (cloak.progress >= 32.0F) {
    // Full-fade ghost: hull_alpha carries the brightness-0x1e source weight,
    // so leave the hull untinted rather than darkening it a second time.
    hull_opts.tint_rgb5 = std::nullopt;
  } else {
    hull_opts.tint_rgb5 =
        std::array<std::int16_t, 3>{tint.red, tint.green, tint.blue};
  }
  ApplyFog(hull_opts);
  if (!cloak.hidden && hull_opts.alpha_mod > 0.0F) {
    DrawSprite(platform.renderer(),
               sprite.base,
               frame,
               hull_x,
               hull_y,
               camera_x,
               camera_y,
               viewport_w,
               viewport_h,
               hull_opts);
  }
  // Engine-glow layer, drawn over the hull with the same heading-selected frame
  // and a thrust-driven additive intensity (NovaShip_TickWeaponSpriteAnd-
  // RunningLights folds the per-ship fog into it for NPCs).
  if (sprite.has_glow && !sprite.glow.frames.empty() &&
      ship.engine_glow_intensity > 0.0F && cloak.effect_cap > 0.0F) {
    SpriteDrawOptions opts;
    // engine_glow_intensity is normalized 0..1 while effect_cap is the
    // original's 0..32 tint units, so compare in the same scale.
    opts.alpha_mod =
        std::min(ship.engine_glow_intensity, cloak.effect_cap / 32.0F) *
        (1.0F - emergence.white_mix);
    opts.additive = true;
    ApplyFog(opts);
    DrawSprite(platform.renderer(),
               sprite.glow,
               frame,
               hull_x,
               hull_y,
               camera_x,
               camera_y,
               viewport_w,
               viewport_h,
               opts);
  }
  // Running lights and weapon effects over the hull (additive, intensity from
  // the visual tick). Every composite layer shares the hull's cloak jitter
  // (0x0042b0b2 writes the same offset into each Sprite rect).
  if (sprite.has_light) {
    DrawShipEffectLayer(platform.renderer(),
                        sprite.light,
                        frame,
                        hull_x,
                        hull_y,
                        camera_x,
                        camera_y,
                        viewport_w,
                        viewport_h,
                        std::min(ship.light_intensity, cloak.effect_cap) *
                            (1.0F - emergence.white_mix),
                        /*visible_threshold=*/1.0F,
                        fog_murk_);
  }
  if (sprite.has_weapon) {
    DrawShipEffectLayer(
        platform.renderer(),
        sprite.weapon,
        frame,
        hull_x,
        hull_y,
        camera_x,
        camera_y,
        viewport_w,
        viewport_h,
        std::min(ship.weapon_sprite_flash_level, cloak.weapon_cap) *
            (1.0F - emergence.white_mix),
        /*visible_threshold=*/0.0F,
        fog_murk_);
  }
  // Alt overlay sheet (sh\x8an AltImageID), an ordinary blend over the hull.
  // The frame is alternate_sprite_cycle_index * FramesPer + heading_frame,
  // advanced by NovaShip_TickSpriteAnimation.
  if (sprite.has_alt && !sprite.alt.frames.empty() && layer_alpha > 0.0F &&
      !cloak.hidden) {
    const int alt_frame =
        ComposeShipFrameIndex(ship,
                              ship.alternate_sprite_cycle_index,
                              sprite.frames_per_rotation,
                              std::max(1, sprite.alt_set_count));
    SpriteDrawOptions opts;
    opts.alpha_mod = layer_alpha * cloak.hull_alpha;
    if (cloak.additive) {
      // The alt sheet copies the hull's cloak tint/jitter (0x0042b4b0).
      opts.additive = true;
      opts.tint_rgb5 = cloak.hull_tint;
    } else if (cloak.progress >= 32.0F) {
      opts.tint_rgb5 = std::nullopt;
    }
    ApplyFog(opts);
    DrawSprite(platform.renderer(),
               sprite.alt,
               alt_frame,
               hull_x,
               hull_y,
               camera_x,
               camera_y,
               viewport_w,
               viewport_h,
               opts);
  }
  // Shield bubble: the sheet is loaded (ShipSpriteSet::shield) but the draw is
  // deferred. The original sets its frame and RGB tint from
  // shield_bubble_flash_intensity, decaying it by g_avg_frame_tick_scale; the
  // tick's shield arm is not reconstructed yet.
  // TODO(decomp(0x00428340)): shield-bubble sprite frame + tint draw.

  // Emergence tint is the final ship-composite pass. Drawing it after the
  // ordinary glow/light/weapon/alt layers is essential: putting those colored
  // layers on top makes the nominally white reveal visibly colored.
  if (emergence.white_mix > 0.0F && emergence.hull_alpha > 0.0F) {
    SpriteDrawOptions white_opts;
    white_opts.alpha_mod = emergence.hull_alpha * emergence.white_mix;
    white_opts.white_silhouette = true;
    DrawSprite(platform.renderer(),
               sprite.base,
               frame,
               ship.pos_x,
               ship.pos_y,
               camera_x,
               camera_y,
               viewport_w,
               viewport_h,
               white_opts);
  }
}

// Draws every active ship in the current system that belongs to `layer` at its
// world position, with its heading/flag-selected frame. Mirrors the original
// ship container assignment in Ship_UpdateVisualState 0x00428340: disabled
// ships (Ship_IsShipDisabled) go to the disabled layer, otherwise a
// squad_leader_ship_slot of 0 marks a player escort, and everything else
// (including the player) shares the normal ship layer. The player is slot 0 and
// is drawn at the viewport centre because the camera is centred on it.
void SpaceflightView::DrawShipsInLayer(SdlPlatform &platform,
                                       const GameState &state,
                                       ShipDrawLayer layer) {
  const Viewport vp = CurrentViewport(platform);
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  for (std::size_t slot = 0; slot < GameState::kMaxShips; ++slot) {
    const Ship &ship = state.ShipAt(slot);
    if (!ship.is_active ||
        ship.current_system_id != state.player.current_system_id) {
      continue;
    }
    ShipDrawLayer ship_layer = ShipDrawLayer::Normal;
    if (NovaAiShip_IsDisabled(state, ship)) {
      ship_layer = ShipDrawLayer::Disabled;
    } else if (ship.squad_leader_ship_slot == 0) {
      ship_layer = ShipDrawLayer::Escort;
    }
    if (ship_layer != layer) {
      continue;
    }
    const std::int16_t class_id =
        static_cast<std::int16_t>(ship.ship_class_id + 0x80);
    const ShipSpriteSet *sprite = ShipSprites(platform, class_id);
    if (sprite == nullptr) {
      continue;
    }
    DrawShipSprite(
        platform, state, ship, *sprite, vp.w, vp.h, camera_x, camera_y);
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

// @port 0x0046ebf0 90% rendering
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
// @port 0x0046ede0 70% rendering
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
    s.frame = RandomBelow(state.rng, frame_count);
    // Random world offset within the (player-centred) viewport.
    const auto rx = static_cast<float>(RandomBelow(state.rng, vp.w));
    const auto ry = static_cast<float>(RandomBelow(state.rng, vp.h));
    s.pos_x = rx + state.player.pos_x - static_cast<float>(vp.w) / 2.0F;
    s.pos_y = ry + state.player.pos_y - static_cast<float>(vp.h) / 2.0F;
    // Ghidra: speed = NovaRandom_Range(0x23) * 0.01 when the motion toggle is
    // on, else forced to 0 (stationary field).
    s.speed =
        kStarfieldMotionEnabled
            ? static_cast<float>(RandomBelow(state.rng, 0x23)) * kSpeedScale
            : 0.0F;
  }
}

// @port 0x0046ee50 85% rendering
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
// @port 0x0042e590 85% rendering,divergence
// DIVERGENCE(original): the 8-bit-depth star path (brightness=t, tint=0) is
// not modelled; the port always uses the 16bpp additive-tint path.
// Frame_UpdateViewportWrapBackgroundSprites [0x0042e590]
// (+ the sprite-world draw in Frame_SpaceflightLoop scope 2) renders the stars.
// Each star draws its randomly-chosen frame of the 16-frame star-field sprite
// sheet (sp\x9an 700, 4x4 grid of 5x5 tiles) at native 1:1 size; a small point
// stands in only if the sheet cannot load.
// @port 0x0046BBF0 60% rendering
// Ghidra 0x0046bbf0 NovaRender_SetSystemSpaceBackgroundColor. The port applies
// the per-system BkgndColor tint in DrawBackground; the original DrawContext
// sprite-effect surface plumbing is not reconstructed.
// @port 0x00497df0 50% rendering
// Ghidra 0x00497df0 Frame_RenderViewportBackground: the per-system backdrop
// clear + fill is ported here; the original's surface/rect-copy plumbing is
// not reconstructed (the starfield draw is the 0x0042e590 note above).
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
  //
  // Star murkiness: with murk == 0 the original takes the plain opaque-copy
  // fast path; otherwise it writes t = trunc(murk * 0.9) (double 0.9 at
  // 0x005753c0), clamped to [2, 29], into the star sprite's RGB tint channels
  // at brightness 0x20 / distance 0 (0x0042e590). The tinted span then draws
  // additively: dst + src*t/32. The port maps t to the additive alpha, so each
  // star stays ~5px but a murky system's starfield reads as a hazy glow.
  const int star_murk = sys ? static_cast<int>(sys->murk) : 0;
  // The original multiplies the integer murk by the binary64 constant 0.9 and
  // truncates toward zero (FMUL double at 0x0042e5f6, then the x87 FIST idiom).
  const int star_tint =
      star_murk > 0
          ? std::clamp(
                static_cast<int>(static_cast<double>(star_murk) * 0.9), 2, 0x1d)
          : 0;
  const Viewport vp = CurrentViewport(platform);
  const SpriteAsset *sheet = StarFieldSheet(platform);
  for (const auto &s : ambient_stars_) {
    if (!s.active) {
      continue;
    }
    if (sheet && !sheet->frames.empty()) {
      // Native tile size, centred, with one-exit wraparound; linear filtering
      // so the tiny 5x5 star tiles read as soft glows when scaled. Stars use
      // the murk tint above, not the per-sprite distance fog (the original
      // leaves the star's distance_brightness at 0).
      SpriteDrawOptions opts;
      opts.wrap = true;
      opts.linear_scale = true;
      if (star_tint > 0) {
        opts.additive = true;
        opts.alpha_mod = static_cast<float>(star_tint) / 32.0F;
      }
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

// @port 0x004AC380 90% gameplay
// Ghidra 0x004ac380 Ship_InitializeMainInterface (viewport half-size half). The
// original sets g_viewport_center_x/y to half the play area:
// round((RenderOwner.right - RenderOwner.left - DAT_0088c020) * 0.5) and
// round((RenderOwner.bottom - RenderOwner.top) * 0.5). CurrentViewport already
// excludes the DAT_0088c020 cockpit strip, so the plain halves match (integer
// truncation vs the original's round differs by at most 1px on odd widths).
// TODO(decomp): DAT_0088c020 is the ui_scale-scaled HUD strip width (0xc2 at
// scale 1.0); the port pins 194 because ui_scale plumbing is not modelled yet
// (docs/display_scaling.md WP3).
void SpaceflightView::SyncGameplayViewport(SdlPlatform &platform,
                                           GameState &state) {
  const Viewport vp = CurrentViewport(platform);
  state.viewport_center_x = vp.w / 2;
  state.viewport_center_y = vp.h / 2;
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
  NovaEffects_TickSwParticles(state, elapsed_ticks);
  NovaFreeflight_Tick(state, elapsed_ticks);
  // Asteroid / drift-debris integration (Asteroid_UpdateSprites 0x00436910
  // simulation half) plus its viewport wrap. The original runs the whole
  // function in Frame_TickSystems scope 8; this port keeps the pure state
  // advance in GameState and the SDL-dependent sprite bind/frame/wrap on the
  // view.
  NovaAsteroid_UpdateSprites(state, elapsed_ticks);
  WrapAsteroids(platform, state);
  // The original hull has no destruction opacity fade: Ship_UpdateVisualState
  // keeps the sprite visible until the Explode2 finale calls
  // Sprite_SetVisible(ship, 0). The port therefore draws the wreck at full
  // opacity until NovaShip_RunShipDestructionFinale deactivates it.
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
  const ShipSpriteSet *debris = ShipSprites(platform, 0x2ff);
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
    ApplyFog(options);
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

// FreeflightObjectState draw half of Frame_UpdateFreeflightObjectSprites
// (0x0042c1b0): each live object draws its 500+index spin set at its world
// position, frame-selected by the tick's frame accumulator and alpha-faded
// over the final 32 ticks. The simulation half (position, lifetime, frame
// counter) lives in NovaFreeflight_Tick. Objects from another system are
// skipped; the original instead clears the whole pool via DAT_00596d2a on
// transitions.
void SpaceflightView::DrawFreeflightObjects(SdlPlatform &platform,
                                            const GameState &state) {
  const Viewport vp = CurrentViewport(platform);
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  for (const FreeflightObjectState &object : state.freeflight_objects) {
    if (object.lifetime_ticks < 0.0F ||
        object.system_id != state.player.current_system_id) {
      continue;
    }
    const SpriteAsset *set =
        sprite_store_.Spin(platform.renderer(),
                           NovaFreeflightSpriteSetId(object.sprite_set_index));
    if (set == nullptr || set->frames.empty()) {
      continue;
    }
    int frame = static_cast<int>(object.frame_counter);
    frame %= set->frame_count;
    if (frame < 0) {
      frame += set->frame_count;
    }
    SpriteDrawOptions options;
    options.alpha_mod = std::clamp(object.lifetime_ticks / 32.0F, 0.0F, 1.0F);
    ApplyFog(options);
    DrawSprite(platform.renderer(),
               *set,
               frame,
               object.pos_x,
               object.pos_y,
               camera_x,
               camera_y,
               vp.w,
               vp.h,
               options);
  }
}

// Ghidra 0x00436910 Asteroid_UpdateSprites (draw half). Each live asteroid /
// drift-debris record draws its type's spin sprite set (id 800 + wander_type).
// The original binds the set with Sprite_AssignSpriteSet and then cycles the
// current frame from the record's wander accumulator (+0x14) wrapped by the
// set's frame count; all shipped asteroid sets are 50x50 with 36 frames. The
// distance-intensity tint and the off-screen wrap are handled next to this
// (WrapAsteroids) / documented as a divergence (no ship-centric fog in this
// port yet).
void SpaceflightView::DrawAsteroids(SdlPlatform &platform,
                                    const GameState &state) {
  if (state.no_asteroids_latch) {
    return;
  }
  const Viewport vp = CurrentViewport(platform);
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  for (const AsteroidState &m : state.asteroid_pool) {
    if (!m.active) {
      continue;
    }
    // wandering type 0..15 selects the Metal/Ice/Silicates/Metal-rich x
    // size-tier set loaded from spin resource 800+type (DAT_00596c00).
    const std::uint16_t spin_id =
        static_cast<std::uint16_t>(kAsteroidSpinBase + (m.wander_type & 0x0f));
    const SpriteAsset *set = sprite_store_.Spin(platform.renderer(), spin_id);
    if (set == nullptr || set->frames.empty()) {
      continue;
    }
    // Sprite_SetCurrentFrame(ROUND(wander_frame_accumulator)) wrapped by the
    // set's frame
    // count. Wrap the accumulator into [0, frame_count) first so a negative
    // drift matches the original's add-frame-count loop.
    const int frame_count = set->frame_count;
    float wander = m.wander_frame_accumulator;
    if (frame_count > 0) {
      while (wander < 0.0F) {
        wander += static_cast<float>(frame_count);
      }
      while (wander >= static_cast<float>(frame_count)) {
        wander -= static_cast<float>(frame_count);
      }
    }
    int frame = static_cast<int>(wander);
    frame = std::clamp(frame, 0, std::max(0, frame_count - 1));
    SpriteDrawOptions options;
    ApplyFog(options);
    DrawSprite(platform.renderer(),
               *set,
               frame,
               m.target_pos_x,
               m.target_pos_y,
               camera_x,
               camera_y,
               vp.w,
               vp.h,
               options);
  }
}

// Ghidra 0x00436910 Asteroid_UpdateSprites (viewport-cull half). The original
// places each record via Sprite_SetPositionFromCurrentFrameAnchor (QuickDraw
// placed bounds: +0x1c left, +0x1a top), then tests the placed left/top against
// [-(2*frame_extent+0x20), 2*g_viewport_center+0x20] using the record's OWN
// frame extent. A record that leaves that window has target_pos moved to the
// opposite side using the largest loaded frame span, then is hidden and
// DEACTIVATED; Frame_TickSystems refills the quota every tick through
// Asteroid_Spawn(place_in_ring=1), placing the refill on a ring of radius
// ~1.5*center_y (see NovaAsteroid_Spawn).
//
// DIVERGENCE (documented, deliberate): the clean-room has no per-record Sprite
// handle and does not model the despawn + ring-refill cycle, so it wraps in
// world space instead -- teleporting a record past the edge to the opposite
// side and never deactivating it -- and uses the largest loaded frame span for
// both the threshold and the teleport distance. The original's negative-side
// cull extent is 1.5x the record's own span versus 1.0x here, and its
// positive-side extent is 0.5x versus 1.0x here, so the keep-alive window and
// refill timing do not match. Consequence: the original's "small asteroids
// vanish at high resolution" defect (ring refills landing outside the cull
// window, worked around by padding frames to ~100x100) is masked rather than
// faithfully reproduced. BUGFIX(original): the port therefore does not exhibit
// the defect (docs/known_original_bugs.md), but this is a renderer-structural
// divergence -- there is no per-record Sprite to gate -- so it is not routed
// through kApplyOriginalBugFixes. Plausibly identified; not a faithful port.
void SpaceflightView::WrapAsteroids(SdlPlatform &platform, GameState &state) {
  // Ghidra 0x00436910 Asteroid_UpdateSprites culls a record that left the
  // play area against g_viewport_center_x/y; use the synced half-size rather
  // than recomputing it, so the wrap matches the scatter/spawn centre.
  const float center_x = static_cast<float>(state.viewport_center_x);
  const float center_y = static_cast<float>(state.viewport_center_y);
  // Sprite_GetFrameFullWidth / Sprite_GetFrameFullHeight return the full
  // frame span; the original scans every loaded asteroid set for the maximum.
  int max_span_x = 1;
  int max_span_y = 1;
  for (int type = 0; type < 16; ++type) {
    const SpriteAsset *set = sprite_store_.Spin(
        platform.renderer(),
        static_cast<std::uint16_t>(kAsteroidSpinBase + type));
    if (set != nullptr && !set->frames.empty()) {
      max_span_x = std::max(max_span_x, set->tile_width);
      max_span_y = std::max(max_span_y, set->tile_height);
    }
  }
  for (AsteroidState &m : state.asteroid_pool) {
    if (!m.active) {
      continue;
    }
    const float edge_x = center_x + static_cast<float>(max_span_x) + 32.0F;
    const float edge_y = center_y + static_cast<float>(max_span_y) + 32.0F;
    const float dx = m.target_pos_x - state.player.pos_x;
    const float dy = m.target_pos_y - state.player.pos_y;
    if (dx > edge_x) {
      m.target_pos_x =
          state.player.pos_x - center_x - static_cast<float>(max_span_x) * 2.0F;
    } else if (dx < -edge_x) {
      m.target_pos_x =
          state.player.pos_x + center_x + static_cast<float>(max_span_x) * 2.0F;
    }
    if (dy > edge_y) {
      m.target_pos_y =
          state.player.pos_y - center_y - static_cast<float>(max_span_y) * 2.0F;
    } else if (dy < -edge_y) {
      m.target_pos_y =
          state.player.pos_y + center_y + static_cast<float>(max_span_y) * 2.0F;
    }
  }
}

// @port 0x0042cd10 85% rendering,gameplay
// Ghidra Stellar_UpdateStellarSprites (0x0042cd10), ambient-animation part.
// Advances each animate stellar of the current system one animation step each
// real frame. Stellar bodies with only a single frame (sprite_frame_count<2)
// stay static at frame 0. For ordinary animated stellars (availability_flags &
// 0x1000 clear) this is the ping-pong/alternate/random cycling stepper; for
// hypergate-style stellars (0x1000 set) it is the non-engaged frame-drift
// toward/around the CustPicID transition frame. Player travel engagement and
// NPC state 0x01/0x14/0x15 drive the opening/working half. frame_time_ms is
// converted to the original normalized 30 Hz tick accumulator basis.
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
    // stellar is not active, link_b otherwise (Stellar_UpdateStellarSprites
    // 0x0042cd10). NovaTargeting_StellarSpriteLinkId centralises that choice so
    // the animated/drawn set matches the collision mask binding.
    const SpriteAsset *set =
        sprite_store_.Spin(platform.renderer(),
                           static_cast<std::uint16_t>(
                               NovaTargeting_StellarSpriteLinkId(*st) + 1000));
    if (!set || set->frame_count < 2) {
      continue; // no animated set / single-frame body stays static
    }
    StellarAnimationState &anim = stellar_anims_[nav];
    const int frame_count = set->frame_count;
    const bool active = NovaTargeting_IsStellarActive(*st);
    const bool animate_when_active =
        (st->availability_flags & Stellar::kAnimateWhenDestroyed) != 0;
    const bool animate = active == animate_when_active;
    if (animate) {
      const bool engaged =
          (st->availability_flags & Stellar::kHypergate) != 0 &&
          IsHypergateAnimationEngaged(state,
                                      nav,
                                      *st,
                                      anim.current_frame,
                                      frame_count,
                                      set->tile_width);
      NovaStellar_AdvanceAnimationFrame(
          state, *st, frame_count, engaged, frame_time_ms * 0.03F, anim);
    }
    // Publish the frame the view draws into the StellarDef-equivalent field
    // (Ghidra StellarDef +0x476) so NovaCollision_RefreshCollisionMasks binds
    // the matching collision mask for this and the next tick.
    if (Stellar *const mutable_stellar = state.scenario.StellarMutable(nav)) {
      mutable_stellar->sprite_current_frame =
          static_cast<std::int16_t>(animate ? anim.current_frame : 0);
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
      continue;
    }
    // Pick an 8-bit tint: the stellar's government (decoded) if present,
    // else the system government.
    std::uint8_t r = 170, g = 170, b = 200;
    const auto *gov =
        st->government_id >= 0
            ? state.scenario.GovernmentByIndex(st->government_id)
            : state.scenario.GovernmentByIndex(sys->government_id);
    if (gov && gov->present) {
      r = gov->theme_red;
      g = gov->theme_green;
      b = gov->theme_blue;
    }

    // Prefer the real spin planet sprite; fall back to a tinted disc. Animated
    // stellars use the frame advanced by AdvanceStellarAnimation (keyed by the
    // stellar id), which also publishes it to Stellar.sprite_current_frame for
    // the collision refresh; the link_a/link_b set choice is shared with the
    // collision refresh via NovaTargeting_StellarSpriteLinkId so the drawn art
    // and the collision mask never disagree.
    const SpriteAsset *set =
        sprite_store_.Spin(platform.renderer(),
                           static_cast<std::uint16_t>(
                               NovaTargeting_StellarSpriteLinkId(*st) + 1000));
    if (set && !set->frames.empty()) {
      int frame_idx = 0;
      const auto anim_it = stellar_anims_.find(nav);
      if (anim_it != stellar_anims_.end()) {
        frame_idx =
            std::clamp(anim_it->second.current_frame, 0, set->frame_count - 1);
      }
      SpriteDrawOptions options;
      ApplyFog(options);
      DrawSprite(platform.renderer(),
                 *set,
                 frame_idx,
                 static_cast<float>(st->pos_x),
                 static_cast<float>(st->pos_y),
                 camera_x,
                 camera_y,
                 vp.w,
                 vp.h,
                 options);
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

void SpaceflightView::DrawShots(SdlPlatform &platform,
                                const GameState &state,
                                ShotDrawLayer layer) {
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
  // anim_elapsed at the weapon's animation-frame interval (see
  // weapon.cpp), and here the cycle is wrapped at the shot set's frame count
  // (or held at frame_count-1 for a flags_secondary bit 1 reverse wrap, the
  // last-frame hold Shot_HandleShot applies) before being drawn.
  for (const auto &s : state.active_shots) {
    // The shot's bank slot is a zero-based weapon id; the scenario Weapon()
    // lookup uses the 0x80.. resource-id residue (same convention as
    // NovaWeapon_FirePlayerWeaponBank).
    const Weapon *w = state.scenario.Weapon(static_cast<std::int16_t>(
        static_cast<std::int16_t>(s.weapon_id) + 0x80));
    // The container is fixed at spawn by Shot_SpawnShotFromWeapon 0x0041fd30:
    // mode 9 -> layer 7, mode 1 -> layer 8, mode 4 with owner Flags3 0x0040 ->
    // layer 12 above the ships, everything else -> default layer 9. The four
    // passes in Draw preserve the original append (draw) order.
    const int mode = (w != nullptr) ? w->weapon_mode_code : -2;
    ShotDrawLayer shot_layer = ShotDrawLayer::Default;
    if (s.draws_above_ships) {
      shot_layer = ShotDrawLayer::Mode4Alt;
    } else if (mode == 9) {
      shot_layer = ShotDrawLayer::Mode9;
    } else if (mode == 1) {
      shot_layer = ShotDrawLayer::Mode1;
    }
    if (shot_layer != layer) {
      continue;
    }
    const SpriteAsset *set =
        (w != nullptr) ? sprite_store_.Spin(
                             platform.renderer(),
                             static_cast<std::uint16_t>(w->sprite_id + 3000))
                       : nullptr;
    if (set && !set->frames.empty()) {
      SpriteDrawOptions opts;
      opts.wrap = false;
      ApplyFog(opts);
      // Ghidra Shot_HandleShot 0x00435f29 pre-subtracts
      // floor(Sprite_GetFrameFullWidth/2) from BOTH axes: the disassembly
      // reads the x-extent accessor (0x00462390) once and reuses it for x and
      // y, then places the frame's zero anchor. Passing that explicit anchor
      // reproduces the original placement (including its non-square-frame
      // quirk); DrawSprite's default frame-centre anchor would instead use the
      // height on y. The original also snaps the world-relative position to a
      // short; the port keeps sub-pixel float positioning (renderer
      // divergence).
      opts.anchor_x = static_cast<float>(set->tile_width / 2);
      opts.anchor_y = opts.anchor_x;
      int frame;
      if ((w->flags & 0x0001U) == 0) {
        // Static/heading branch: Shot_HandleShot computes the frame as
        // trunc(frame_count * heading_deg / 360), using the stored flight
        // heading (not the velocity direction, which a guided shot may differ
        // from).
        const float sector =
            s.heading_deg * static_cast<float>(set->frame_count) / 360.0F;
        frame = static_cast<int>(std::trunc(sector)) % set->frame_count;
        if (frame < 0) {
          frame += set->frame_count;
        }
      } else {
        // Time-animated branch: the displayed frame is the value latched
        // BEFORE the animation increment (Shot_HandleShot snapshots
        // frame_cycle_index first). Clamp it at the set's frame count, holding
        // the last frame for a flags_secondary bit 1 reverse-wrap.
        int cycle = s.display_frame;
        if (cycle >= set->frame_count) {
          cycle =
              ((w->flags_secondary & 0x0002U) != 0) ? set->frame_count - 1 : 0;
        }
        frame = std::clamp(cycle, 0, set->frame_count - 1);
      }
      // Ghidra Shot_HandleShot shot_fade_rate quads: fade_alpha is the latched
      // pre-advance per-corner intensity. flags_tertiary 0x2 selects the
      // additive tint path instead of ordinary alpha blending.
      opts.alpha_mod *= std::clamp(s.fade_alpha, 0.0F, 1.0F);
      opts.additive = s.fade_additive;
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

void SpaceflightView::DrawSmokePuffs(SdlPlatform &platform,
                                     const GameState &state) {
  SDL_Renderer *const renderer = platform.renderer();
  const Viewport vp = CurrentViewport(platform);
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  for (const WeaponSmokePuff &puff : state.weapon_smoke_puffs) {
    const int frame = NovaWeapon_SmokePuffFrame(puff);
    // The original loader (0x004ae7fe) builds only SmokeSet 0 and 1 (resource
    // bases 1000/1008); slots 2..7 have a null sprite set, so a puff of any
    // higher set is invisible even if a plug-in ships cicn 1016+. Match that
    // exactly instead of loading a set the original never creates.
    if (frame < 0 || puff.effect_slot < 0 || puff.effect_slot > 1) {
      continue;
    }
    const auto slot = static_cast<std::size_t>(puff.effect_slot);
    if (!smoke_puff_sets_tried_[slot]) {
      smoke_puff_sets_tried_[slot] = true;
      smoke_puff_sets_[slot] = SpriteAsset::LoadCicnSet(
          renderer, static_cast<std::uint16_t>(1000 + puff.effect_slot * 8), 8);
    }
    const SpriteAsset *set = smoke_puff_sets_[slot].get();
    if (set == nullptr || set->frames.empty()) {
      continue;
    }
    SpriteDrawOptions opts;
    opts.wrap = false;
    ApplyFog(opts);
    // Ghidra Shot_UpdateWeaponSmokePuffs 0x0042c660 positions each puff with
    // floor(full_width/2) on x and floor(full_height/2) on y. The cicn sets
    // carry a (0,0) frame anchor (they are shared with the reticle brackets),
    // so the half-span must be supplied here; the original forces the four
    // corner words to 0x20 (full) afterwards, which full alpha + ApplyFog
    // reproduces.
    opts.anchor_x = static_cast<float>(set->tile_width / 2);
    opts.anchor_y = static_cast<float>(set->tile_height / 2);
    DrawSprite(renderer,
               *set,
               std::clamp(frame, 0, set->frame_count - 1),
               puff.pos_x,
               puff.pos_y,
               camera_x,
               camera_y,
               vp.w,
               vp.h,
               opts);
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
    const int frame = std::clamp(
        static_cast<int>(instance.anim_time), 0, set->frame_count - 1);
    SpriteDrawOptions opts;
    // Ghidra computes distance_intensity (+0xa2) as 32 - anim_time/frame_count
    // for impact sprites. The SDL renderer expresses that as normalized alpha.
    opts.alpha_mod = std::clamp(1.0F - instance.anim_time /
                                           static_cast<float>(set->frame_count),
                                0.0F,
                                1.0F);
    ApplyFog(opts);
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

// @port 0x0047bdd0 80% rendering,bugfix
// SDL alpha deliberately follows the original 16-bit soft-particle path,
// clamping life/32 to full opacity.
// TODO(decomp): dirty-pixel save/restore is not reproduced.
// Ghidra SWParticles_DrawParticles (0x0047bdd0): the post-render single-pixel
// particle pass. The original projects each particle from its 8.8 fixed world
// position onto the gameplay surface and writes one pixel per particle. This
// port draws a 1x1 logical-pixel SDL point (SDL scales it by the display
// density, so retina gets a 2x2 backing block like every other 1:1 sprite).
// SDL alpha reproduces the original 16-bit soft-particle blend: particles with
// fewer than 32 life ticks contribute life/32 of their color. The original
// 32-bit software-surface branch wrote opaque pixels, but that makes ordinary
// weapon sparks (typically only 8-10 ticks) stay fully bright until they pop
// out in the SDL renderer.
void SpaceflightView::DrawSwParticles(SdlPlatform &platform,
                                      const GameState &state) {
  if (state.sw_particles.empty()) {
    return;
  }
  SDL_Renderer *renderer = platform.renderer();
  const Viewport vp = CurrentViewport(platform);
  const auto [camera_x, camera_y] = WorldCameraPosition(state);
  const float half_w = static_cast<float>(vp.w) / 2.0F;
  const float half_h = static_cast<float>(vp.h) / 2.0F;
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  for (const SwParticle &particle : state.sw_particles) {
    // The original draw skips entries with life <= 1.
    if (particle.life_ticks <= 1) {
      continue;
    }
    const float world_x = static_cast<float>(particle.pos_x) / 256.0F;
    const float world_y = static_cast<float>(particle.pos_y) / 256.0F;
    const float screen_x = world_x - camera_x + half_w;
    const float screen_y = world_y - camera_y + half_h;
    if (screen_x < 0.0F || screen_x >= static_cast<float>(vp.w) ||
        screen_y < 0.0F || screen_y >= static_cast<float>(vp.h)) {
      continue;
    }
    const std::uint8_t red = static_cast<std::uint8_t>(particle.color >> 16U);
    const std::uint8_t green = static_cast<std::uint8_t>(particle.color >> 8U);
    const std::uint8_t blue = static_cast<std::uint8_t>(particle.color);
    constexpr int kFullParticleWeight = 0x20;
    const int life_weight =
        std::clamp<int>(particle.life_ticks, 0, kFullParticleWeight);
    int alpha = life_weight * SDL_ALPHA_OPAQUE / kFullParticleWeight;
    // BUGFIX(original): the executable never applies the system murkiness
    // distance fog to SWParticles, so weapon sparks and asteroid debris stay
    // full-bright at any range even in a murky system. Dim them with the same
    // distance_brightness the sprite layers use; alpha-only fading is enough
    // here because particles composite over whatever is behind them (unlike
    // the opaque ship silhouette, there is nothing to occlude). Gated by
    // kApplyOriginalBugFixes; see docs/system_murk_rendering.md.
    if (kApplyOriginalBugFixes && fog_murk_ > 0) {
      const int distance_brightness = Sprite_DistanceBrightness(
          fog_murk_, camera_x, camera_y, world_x, world_y);
      // d >= 0x1f is fully fogged (matches the sprite fog's ceiling snap).
      const int fog = distance_brightness >= kFullParticleWeight
                          ? 0
                          : kFullParticleWeight - distance_brightness;
      alpha = alpha * fog / kFullParticleWeight;
    }
    SDL_SetRenderDrawColor(renderer, red, green, blue, alpha);
    SDL_RenderPoint(renderer, screen_x, screen_y);
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// @port 0x00438c40 85% rendering,synthetic
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

// @port 0x00438810 50% rendering
// Ghidra Shot_DrawBeamHitQueueForSurface (0x00438810), visible path: the
// topmost gameplay layer draws every beam that does NOT set flags_secondary
// 0x2000 (those render underneath ships via DrawBeamsUnderShips). The
// field_0xec != 0 twin-surface branches are not reproduced yet: they replot
// every live beam to both gameplay surfaces via SWBeams_DrawKinkedBeam
// (0x0047AC50) / SWBeams_DrawBeamWithFlare (0x0047AFD0), including the
// under-ships twin branch, instead of the single-surface DrawShortBeam /
// ThickFadingBeam appearance the port always uses. The restore-phase twin
// Shot_DrawBeamQueue (0x004386f0) has no SDL equivalent (the port keeps no
// saved-backdrop buffer).
// TODO(decomp(0x00438810)): twin-surface branches and their plotters.
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

// Fills a sprite draw's fog fields from the current frame's effective murk and
// space colour (see SpriteDrawOptions.fog_murk).
void SpaceflightView::ApplyFog(SpriteDrawOptions &opts) const {
  opts.fog_murk = fog_murk_;
  opts.fog_color = fog_color_;
}

// Draws the whole in-flight world. The original builds 14 ordered SpriteLayer
// objects in NovaUi_InitializeFlightViewSurfaces (0x004ab9d4) and draws them
// head-to-tail in SpriteWorld_RenderLayers (0x00477fa0), so append order is
// draw order; ships additionally hop between the disabled/escort/normal ship
// layers at runtime (Ship_UpdateVisualState 0x00428340). Original order, bottom
// to top: ambient stars -> stellars (+ flags-0x2000 under-ships beams) ->
// asteroids -> disabled ships -> freeflight objects + fading effects -> smoke
// puffs -> shots -> escort ships -> ships (player + NPC) -> shots -> impact /
// destruction effects -> directional weapon effects (+ visible beams).
//
// The port locks the precedence explicitly here (rather than spreading it over
// the per-subsystem drawers) as:
//
//   background (space tint + ambient starfield)   -- DrawBackground
//   stellar bodies (planets / stations)            -- DrawStellarBodies
//   under-ships beams (flags_secondary 0x2000)     -- DrawBeamsUnderShips
//   asteroids (layer 3)                            -- DrawAsteroids
//   disabled ships (layer 4)                       --
//   DrawShipsInLayer(Disabled) freeflight objects + fading effects (layer 5) --
//   DrawFreeflightObjects,
//                                                     DrawFadingEffects
//   shots, mode 9 / mode 1 / default (layers 7-9)  -- DrawShots(Mode9),
//                                                     DrawShots(Mode1),
//                                                     DrawShots(Default)
//   escort ships (layer 10)                        -- DrawShipsInLayer(Escort)
//   ships, player + non-escort NPCs (layer 11)     -- DrawShipsInLayer(Normal)
//   turreted-above shots (layer 12)                -- DrawShots(Mode4Alt)
//   destruction / impact effects (layer 13)        -- DrawImpactEffects
//   reticles + over-ships beams (layer 14)         -- DrawShipTargetReticle,
//                                                     DrawTravelTargetReticle,
//                                                     DrawBeamsOverShips
//   weapon sparks / debris (SWParticles)           -- DrawSwParticles
//
// Impact/destruction effects composite over ships (original layer 13 above
// layer 11), so a dying hull's explosions cover the wreck. Beams render in two
// original layers: flags-0x2000 beams on layer 2 (above the backdrop only,
// Ghidra 0x00438c40), after that layer's stellar sprites, and every other beam
// on topmost layer 14 (Ghidra Shot_DrawBeamHitQueueForSurface 0x00438810).
// Ships hop between the disabled/escort/normal layers exactly as
// Ship_UpdateVisualState 0x00428340 does, and shots keep their original
// per-mode containers (mode9/mode1/default on layers 7-9, mode4_alt on layer
// 12). The layer-6 weapon smoke-puff pool is ported and drawn (DrawSmokePuffs,
// Shot_UpdateWeaponSmokePuffs 0x0042c660) but no stock weapon enables it.
// Remaining divergence: the layer-14 beam pass still lacks the twin-surface
// branches.
void SpaceflightView::Draw(SdlPlatform &platform, const GameState &state) {
  // Effective system murk (raw system murk + owned MurkMod outfits), rebuilt
  // for this frame's distance fog. Ghidra caches it in
  // g_distance_intensity_scale via Outfit_RecomputeOutfitDerivedState;
  // recomputing here keeps it correct across system changes without depending
  // on the recompute call sites. The fog target is the system's BkgndColor, so
  // a fully fogged sprite collapses to the backdrop colour.
  fog_murk_ = NovaSystem_GetEffectiveMurkPercent(state);
  if (const auto *sys = state.scenario.System(
          static_cast<std::int16_t>(state.player.current_system_id + 0x80))) {
    fog_color_ = sys->bkgnd_color;
  } else {
    fog_color_ = 0;
  }
  DrawBackground(platform, state); // backmost: tint + ambient stars
  // Original sprite layer 2: stellar sprites first, then the layer's
  // under-ships beam callback (flags_secondary 0x2000) runs after them.
  DrawStellarBodies(platform, state);   // stellar planets / stations
  DrawBeamsUnderShips(platform, state); // flags-0x2000 beams, below ships
  DrawAsteroids(platform, state);       // layer 3: drifting asteroid field
  // Layer 4: disabled ships (Ship_IsShipDisabled) sit below the freeflight
  // objects, projectiles and active ships.
  DrawShipsInLayer(platform, state, ShipDrawLayer::Disabled);
  // Layer 5: freeflight objects are appended before fading effects, so they
  // sit below them; both are below shots and ships.
  DrawFreeflightObjects(platform, state); // jettisoned pods / launched drones
  DrawFadingEffects(platform, state);     // directional destruction fragments
  DrawSmokePuffs(platform, state);        // layer 6: weapon smoke puffs
  // Layers 7-9: the mode9, mode1 and default shot containers, in that order,
  // below the ships.
  DrawShots(platform, state, ShotDrawLayer::Mode9);
  DrawShots(platform, state, ShotDrawLayer::Mode1);
  DrawShots(platform, state, ShotDrawLayer::Default);
  // Layer 10: player escorts (squad_leader_ship_slot == 0) draw below the
  // player and the non-escort NPCs (layer 11). The player is slot 0 and lands
  // at the viewport centre because the camera is centred on it; the death
  // explosions (layer 13) then composite over the wreck.
  DrawShipsInLayer(platform, state, ShipDrawLayer::Escort);
  DrawShipsInLayer(platform, state, ShipDrawLayer::Normal);
  // Layer 12: mode-4 shots from Flags3-0x0040 ships (Bible "ship's turreted
  // shots appear above the ship") composite over the ships.
  DrawShots(platform, state, ShotDrawLayer::Mode4Alt);
  DrawImpactEffects(platform, state);       // layer 13: explosions over ships
  DrawShipTargetReticle(platform, state);   // target brackets over the ships
  DrawTravelTargetReticle(platform, state); // brackets over the travel target
  DrawBeamsOverShips(platform, state);      // topmost layer: normal beams

  // Post-render particle pass (Ghidra Frame_PresentViewportAndParticles draws
  // SWParticles after the sprite world, so they composite over the ships).
  DrawSwParticles(platform, state);
}

// @port 0x0042ede0 92% rendering,correctness
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
  int frame_base = 4; // neutral or other target
  if (NovaAiShip_IsDisabled(state, target)) {
    frame_base = 0xc;
  } else {
    if (target.squad_leader_ship_slot == 0 &&
        target.defense_fleet_home_stellar_id == -1) {
      frame_base = 8; // directly targeting the player
    } else {
      frame_base = NovaTargeting_IsThreatToPlayerSquad(state, target) ? 0 : 4;
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
  // pulse. Sprite_GetFrameFullWidth / Sprite_GetFrameFullHeight return the
  // target's full frame width/height, so `full` is max(w, h), then the game
  // halves it rounding up; the pulse term is truncated to whole pixels
  // exactly as the original does. The fallback uses the sheet's native tile
  // size.
  float full = 32.0F; // default Sprite_Get*HalfSpan for a missing sprite
  if (const ShipSpriteSet *sprite = ShipSprites(
          platform, static_cast<std::int16_t>(target.ship_class_id + 0x80));
      sprite != nullptr && !sprite->base.frames.empty()) {
    full = std::max(static_cast<float>(sprite->base.tile_width),
                    static_cast<float>(sprite->base.tile_height));
  }
  const float off = std::ceil(full * 0.5F) + std::trunc(pulse);

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

// @port 0x0042eac0 78% ui
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
  // pulse term is truncated to whole pixels (original truncates it too).
  float full = 32.0F; // default Sprite_Get*HalfSpan fallback (0x20)
  if (const auto *spin = sprite_store_.Spin(
          platform.renderer(),
          static_cast<std::uint16_t>(NovaTargeting_StellarSpriteLinkId(*st) +
                                     1000));
      spin && !spin->frames.empty()) {
    full = std::max(static_cast<float>(spin->tile_width),
                    static_cast<float>(spin->tile_height));
  }
  const float off = std::ceil(full * 0.5F) + std::trunc(pulse);

  // Frame base by the destination's hazard marker (Ghidra StellarDef
  // dominated +0x46 in NovaUi_UpdateTravelTargetReticle 0x0042eac0:
  // marker clear -> frames 0..3, set -> frames 4..7 of cicn 10000+).
  const int frame_base = st->dominated ? 4 : 0;
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
    const ShipSpriteSet *sprite = ShipSprites(
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
    // The original hit-tests the live ambient sprite; use the same paint set
    // the renderer/collision selected (link_a, or link_b when active).
    const SpriteAsset *set =
        sprite_store_.Spin(platform.renderer(),
                           static_cast<std::uint16_t>(
                               NovaTargeting_StellarSpriteLinkId(*st) + 1000));
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
  const ShipSpriteSet *sprite = ShipSprites(
      platform, static_cast<std::int16_t>(state.player.ship_class_id + 0x80));
  if (sprite == nullptr || sprite->base.frames.empty()) {
    return false;
  }
  const Viewport vp = CurrentViewport(platform);
  const float wx = rx - static_cast<float>(vp.w) / 2.0F + state.player.pos_x;
  const float wy = ry - static_cast<float>(vp.h) / 2.0F + state.player.pos_y;
  float half_w = static_cast<float>(sprite->base.tile_width) * 0.5F;
  float half_h = static_cast<float>(sprite->base.tile_height) * 0.5F;
  if (sprite->base.tile_height < 0x30) {
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
  platform.SetPlacement(PlaceWindow(platform.logical_playfield_size()));
  Draw(platform, state);
  // HUD overlays the extending world at fixed, unscaled size (the project's
  // resolution policy: more window = more system shown, NOT a bigger HUD).
  hud.Draw(platform, state);
  // Hyperspace flash: a full-screen frame at the jump moment (the original's
  // centered effect 0x32 queued at engage, the 'boom' flash) plus the Mac
  // progressive fade-in during the hold. White by default; the CE build forces
  // black when `hyperspace_effects` is off (0x00872384). Drawn topmost so it
  // also whites/blacks out the HUD, then fades as the loop decays
  // screen_flash_intensity.
  if (state.screen_flash_intensity > 0.0F) {
    SDL_Renderer *const renderer = platform.renderer();
    const std::uint8_t a = static_cast<std::uint8_t>(
        std::clamp(state.screen_flash_intensity, 0.0F, 1.0F) * 255.0F);
    const std::uint8_t v = state.screen_flash_black ? 0 : 255;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, v, v, v, a);
    SDL_RenderFillRect(renderer, nullptr);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
  }
}

} // namespace game
