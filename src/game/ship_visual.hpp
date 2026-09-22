#pragma once

// Clean-room model of Nova's ship-animation (sh\x8an) resource descriptor and
// the per-class sprite data derived from it for the flight rendering path.
//
// Ghidra ShipClass_LoadShipClassVisualAndLaunchData (0x004b4ee0) loads one
// ship's sh\x8an descriptor, reads the Bible `shän` field layout, and builds
// the six per-class sprite sets (base/alt/glow/light/weapon/shield). Only the
// base rotating ship sprite is needed for the in-flight ship draw, so this
// decoder carries the base-image fields and the rotation metadata that drive
// frame selection by heading.

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "game_state.hpp"

namespace game {

// Ghidra g_player_death_timer_scale (0x00575378): the player's death
// presentation timer is seeded at 3x the class DeathDelay. Shared by the
// Ship_UpdateVisualState reseed (NovaShip_TickDestroyedShipVisualState) and
// the hit-site wreck fade timer in collision.cpp.
inline constexpr float kPlayerDeathTimerScale = 3.0F;

// Ghidra g_hull_blast_radius_scale_f64 (0x00575380) / _addend_f64
// (0x00575388) and g_hull_blast_damage_scale_f64 (0x00575390) /
// _addend_f64 (0x00575398). A destroyed hull with Mass >= 100 tons blasts a
// per-axis radius round(Mass*0.075 + 50) and damage round(Mass*0.0375 + 25).
// These are 8-byte doubles in the original (FMUL/FADD double ptr); reading
// them as floats aliases the wrong bytes.
inline constexpr double kHullBlastRadiusScale = 0.075;
inline constexpr double kHullBlastRadiusAddend = 50.0;
inline constexpr double kHullBlastDamageScale = 0.0375;
inline constexpr double kHullBlastDamageAddend = 25.0;

// Resource four-byte type code for the ship-animation descriptor (sh\x8an).
constexpr std::uint32_t kShipVisualResourceType = 0x73688a6e;

// One resolved hull tint: the original's raw 16-bit channels (same units as
// the global ship-paint globals). PersDef/outfit colors are 5-bit (0x20 =
// neutral); the loader stores a government's 8-bit ShipColor as byte << 8.
struct NovaShipTintColor {
  std::int16_t red = 0x20;
  std::int16_t green = 0x20;
  std::int16_t blue = 0x20;
};

// Ghidra 0x0046e470 Ship_ResolveShipTintColor. Defaults to the global paint,
// then for a non-player hull overrides from pers_def_slot color or the faction
// government's ShipColor; all-zero falls back to 0x20.
[[nodiscard]] NovaShipTintColor
NovaShip_ResolveTintColor(const GameState &state, const Ship &ship);

// The per-frame cloak render state derived from the fade progress (Ghidra
// 0x00428340 0x0042b0b2..0x0042b758). The original mutates each ship Sprite's
// brightness (+0xA2) and RGB tint channels (+0xA4/a6/a8) and, for a partial
// fade, nudges the hull rect. The SDL renderer instead folds this into the
// draw options:
//   - partial fade (0 < progress < 32): the hull/alt draw additively
//     (dst + src*tint/32) with per-channel hull_tint = trunc(resolved -
//     progress) clamped to 0/2, the glow/light/weapon intensities capped at
//     effect_cap / weapon_cap, and every composite layer jittered by
//     cloak_jitter_x/y.
//   - full fade (progress >= 32): the hull and alt are not blitted (brightness
//     0x40) unless the player, a player escort (squad_leader_ship_slot == 0),
//     or the player's screen scanner can see it, in which case the hull draws
//     as a faint ghost (hull_alpha); hidden marks the no-blit case. The
//     glow/light/shield layers are hidden, but the weapon layer still draws
//     under the shared 40 - progress cap (0x0042b7f2).
struct ShipCloakPresentation {
  bool hidden = false;   // brightness 0x40: hull/alt blit skipped
  bool additive = false; // partial fade: dst + src*tint/32
  float progress = 0.0F; // 0..32
  std::array<std::int16_t, 3> hull_tint = {0x20, 0x20, 0x20};
  float hull_alpha = 1.0F;  // full-fade ghost source weight
  float effect_cap = 32.0F; // glow/light cap (32 - progress)
  float weapon_cap = 32.0F; // weapon-flash cap (40 - progress)
  float jitter_x = 0.0F;
  float jitter_y = 0.0F;
};

// Ghidra 0x00428340 Ship_UpdateVisualState cloak render slice. `resolved_tint`
// is NovaShip_ResolveTintColor for the hull. Pure; no SDL.
[[nodiscard]] ShipCloakPresentation
NovaShip_CloakPresentation(const GameState &state,
                           const Ship &ship,
                           const NovaShipTintColor &resolved_tint);

// Decoded sh\x8an base-image fields (Bible names in parentheses) plus the
// rotation metadata used to index frames by heading.
struct ShipVisualDescriptor {
  // BaseImageID (+0x00): resource id of the rl\x91D 16-bit sprite sheet that
  // holds this ship's rotating frames.
  std::uint16_t base_image_id = 0;
  // BaseMaskID (+0x02): matching sprite mask (ignored for rl\x91D sheets).
  std::uint16_t base_mask_id = 0;
  // BaseSetCount (+0x04): number of sprite sets in the sheet (frame
  // multiplier), clamped >= 1 by the loader.
  std::int16_t base_set_count = 1;
  // BaseXSize/BaseYSize (+0x06/+0x08): one frame's pixel dimensions.
  std::uint16_t base_x_size = 0;
  std::uint16_t base_y_size = 0;
  // BaseTransp (+0x0a): inherent transparency 0..32 (the loader stores this in
  // ShipClassDef.base_transparency).
  std::int16_t base_transparency = 0;
  // AltImageID/AltMaskID (+0x0c/+0x0e): a separate alternate sprite sheet
  // (Ghidra ShipClass_LoadShipClassVisualAndLaunchData gates it on
  // AltImageID > 0 && AltSetCount > 0); the basic sets themselves all live in
  // the base sheet (Bible BaseSetCount). -1/0 = none on most classes.
  std::int16_t alt_image_id = -1;
  std::uint16_t alt_mask_id = 0;
  // AltSetCount (+0x10): sprite sets of the alternate sheet (Flags 0x0002
  // cycling); NOT extra rows appended to the base sheet.
  std::int16_t alt_set_count = 0;
  // FramesPer (+0x34): rotating frames for one full revolution (loader default
  // 36). Total frames in the sheet = base_set_count * frames_per_rotation.
  std::int16_t frames_per_rotation = 36;
  // Flags (+0x2e): the ship sprite behavior flags (bank/unfold/carry-animate).
  std::uint16_t sprite_behavior_flags = 0;
  // AnimDelay (+0x30): the loader stores this in
  // ShipClassDef.combat_state_init_range.
  std::int16_t anim_delay = 0;
  // WeapDecay (+0x32): the weapon-glow fade rate; scaled into
  // ShipClassDef.weapon_glow_decay_rate.
  std::int16_t weapon_decay = 0;
  // Engine-glow layer. Ghidra ShipClass_LoadShipClassVisualAndLaunchData reads
  // the glow image/mask/x/y from the sh\x8an descriptor at +0x16/+0x18/+0x1a/
  // +0x1c and builds the per-class glow sprite from the named rl\x9144 sheet
  // (for the starter Shuttle: 'Shuttle Eng Glow' 0x0578, 48x48, same rotation
  // grid as the base). The glow X/Y sizes match the sheet canvas (48x48 vs the
  // 24x24 hull), so the exhaust jets extend past the ship. A non-positive image
  // id means the ship has no engine-glow layer (verified on the shuttle:
  // +0x16 = 0x0578).
  std::int16_t engine_glow_image_id = 0; // GlowImageID (+0x16)
  std::int16_t engine_glow_mask_id = 0;  // GlowMaskID (+0x18)
  std::uint16_t engine_glow_x_size = 0;  // GlowXSize (+0x1a)
  std::uint16_t engine_glow_y_size = 0;  // GlowYSize (+0x1c)

  // Running-lights layer (Bible LightImageID). Loader 0x004b4ee0 reads
  // image/mask/x/y at +0x1e/+0x20/+0x22/+0x24 and binds it to the per-class
  // light sprite set; Ship_UpdateVisualState (0x00428340) drives its
  // brightness with the BlinkMode fields below. A non-positive image id means
  // the class has no running lights.
  std::int16_t light_image_id = 0; // LightImageID (+0x1e)
  std::int16_t light_mask_id = 0;  // LightMaskID (+0x20)
  std::uint16_t light_x_size = 0;  // LightXSize (+0x22)
  std::uint16_t light_y_size = 0;  // LightYSize (+0x24)
  // Weapon-effects layer (Bible WeapImageID). Loader +0x26/+0x28/+0x2a/+0x2c;
  // flashed to full brightness by Weapon_FirePlayerWeaponBank /
  // Weapon_FireShipWeapons when the fired weapon carries flags_secondary 0x200,
  // then faded by Ship_UpdateVisualState at weapon_glow_decay_rate.
  std::int16_t weapon_image_id = 0; // WeapImageID (+0x26)
  std::int16_t weapon_mask_id = 0;  // WeapMaskID (+0x28)
  std::uint16_t weapon_x_size = 0;  // WeapXSize (+0x2a)
  std::uint16_t weapon_y_size = 0;  // WeapYSize (+0x2c)

  // Shield-bubble layer (Bible ShieldImageID). Loader 0x004b4ee0 reads
  // image/mask/x/y at +0x40/+0x42/+0x44/+0x46 and builds the per-class shield
  // sprite set (a number of frames equal to 1, FramesPer, or
  // BaseSetCount*FramesPer). Ship_UpdateVisualState 0x00428340 drives its
  // brightness from shield_bubble_flash_intensity. The clean-room renderer
  // loads the sheet but the draw stays deferred (see SpaceflightView).
  std::int16_t shield_image_id = 0; // ShieldImageID (+0x40)
  std::int16_t shield_mask_id = 0;  // ShieldMaskID (+0x42)
  std::uint16_t shield_x_size = 0;  // ShieldXSize (+0x44)
  std::uint16_t shield_y_size = 0;  // ShieldYSize (+0x46)

  // Running-lights blink program. Ghidra's loader names sh\x8an +0x36..+0x3e
  // gun/turret/guided exit positions, but every consumer is the light blink
  // state machine in Ship_UpdateVisualState (0x00428340) and the loader clamps
  // them exactly like Bible BlinkMode 2/3 intensities (0x1f), so the real
  // fields are BlinkMode + BlinkValA..D (Bible order). The gun/turret/guided
  // exit geometry actually begins at +0x48 (decoded as ShipClass.muzzle_*).
  std::int16_t blink_mode = 0;  // BlinkMode (+0x36)
  std::int16_t blink_val_a = 0; // BlinkValA (+0x38), min intensity / off-time
  std::int16_t blink_val_b = 0; // BlinkValB (+0x3a), max intensity / on-time
  std::int16_t blink_val_c = 0; // BlinkValC (+0x3c), blinks per group / delay
  std::int16_t blink_val_d = 0; // BlinkValD (+0x3e), group delay / decay rate

  // Per-turret-group weapon-exit geometry decodes once per class into
  // ShipClass (scenario_data.hpp, muzzle_* fields), using the loader-verified
  // strides from the 0x004b4ee0 copy map and the barrel indexing in
  // Weapon_ApplyTurretSpreadVelocity 0x0046c5c0.
};

// Decodes one sh\x8an descriptor payload (Ghidra ShipClass_LoadShipClass-
// VisualAndLaunchData reads exactly these big-endian fields). Returns nullopt
// when the payload is too small for the header fields.
[[nodiscard]] std::optional<ShipVisualDescriptor>
DecodeShipVisualDescriptor(std::span<const std::byte> resource_data);

// Ghidra 0x00428340 Ship_UpdateVisualState, destruction slice (all hulls,
// including the player). The elapsed-time accumulator is port-only scheduling
// machinery: it invokes the original discrete body once per logical 21 ms
// outer-loop call. Advances one destroyed ship's death presentation and
// runs the once-only destruction finale when the timer enters the (0, 2]
// tick window: blast damage to nearby hulls, the mission DESTRUCTION
// bookkeeping (quick-fail arm + goal_counter_a++ + target_ship_count--), the
// personality deactivation, the Explode2 finale explosion, and the hull
// deactivation. Reseeding when the timer has run out (DeathDelay) matches the
// original; the player's seed is tripled by g_player_death_timer_scale
// (0x00575378). Hulls with DeathDelay 0/1 destruct immediately when
// kApplyOriginalBugFixes is on; with it off they reproduce the original's
// immortal-ghost reseed loop.
void NovaShip_TickDestroyedShipVisualState(GameState &state,
                                           Ship &ship,
                                           float elapsed_ticks);

// Port helper: runs exactly one original-call invocation of the destruction
// slice after its owning ship handler has decremented death_timer_active. It
// is not a recovered original function or an additional simulation pass.
void NovaShip_TickDestroyedShipVisualStateRawCall(GameState &state, Ship &ship);

// Ghidra 0x00428340 Ship_UpdateVisualState, debris-puff window. While the
// death timer is above the finale threshold (2.0) the original rolls
// 1-in-1/2/4/8 by the 20/40/60-tick bands and, on a hit, spawns an Explode1
// area impact at a small random hull offset (randomly silent). This is the
// repeated-explosion cadence of the death presentation; it runs for both NPCs
// (via NovaShip_TickDestroyedShipVisualState) and the player.
void NovaShip_TickDestroyedDebrisPuffs(GameState &state, Ship &ship);

// Ghidra 0x00428340 Ship_UpdateVisualState, destruction finale arm only.
// Runs the once-per-wreck bookkeeping and deactivates the hull; exported for
// the player-ship death path, which ticks its own presentation timer.
void NovaShip_RunShipDestructionFinale(GameState &state, Ship &ship);

// Ghidra 0x00428340 Ship_UpdateVisualState, sprite-frame composition slice
// (all hulls). Advances the flags-selected base-row state machines and the
// separate AltImageID overlay cycle. The renderer composes the displayed
// base/alt frame from the resulting fields at draw time, matching the
// original's Sprite_SetCurrentFrame writes.
//
// Row selection (only active when the base sheet has more than one set, i.e.
// ShipClassDef.base_set_count >= 2):
//   0x0001 banking     -> ai_turn_bias_dir (row 1 left / row 2 right)
//   0x0002 fold/unfold -> waypoint_arrival_marker_b, step timer
//                         turn_bank_animation_phase, AnimDelay per step
//   0x0004 carry       -> row 1 while a launch bay is loaded (draw time)
//   0x0008 combat/part -> sprite_animation_cycle_index, step timer
//                         sprite_animation_timer, AnimDelay per step
// The first four are mutually exclusive. The alt sheet cycles in lockstep with
// the 0x0008 arm when that arm is active, otherwise on its own AnimDelay timer.
void NovaShip_TickSpriteAnimation(GameState &state,
                                  Ship &ship,
                                  float elapsed_ticks);

// Composes the base sprite row for the class's Flags from the ship's current
// animation state. Pure; called by the renderer so the frame stays in sync
// with the tick above without caching a row. Returns 0 for a single base set.
[[nodiscard]] int ComposeShipBaseRow(const GameState &state,
                                     const Ship &ship,
                                     const ShipClass &cls);

// Ghidra 0x00428340 Ship_UpdateVisualState, weapon-effects + running-lights
// slice (all hulls). Decays the weapon-effects sprite flash level set at the
// fire site (Weapon_FirePlayerWeaponBank / Weapon_FireShipWeapons, gated on
// the WeaponDef flags_secondary 0x200 muzzle-flash bit) by the class's
// weapon_glow_decay_rate (WeapDecay * binary64 0.333), and advances the
// running-light blink state machine from the class's BlinkMode / BlinkValA..D.
// When the class carries sprite_behavior_flags 0x0040 (Bible: hide running
// light sprites when the ship is disabled) and the ship is disabled, the
// blink result is overridden to zero so the layer goes dark. The results
// (Ship.weapon_sprite_flash_level, Ship.light_intensity, both 0..32) feed the
// weapon-effects and light sprite layers in the renderer.
void NovaShip_TickWeaponSpriteAndRunningLights(GameState &state,
                                               Ship &ship,
                                               float elapsed_ticks);

// Ghidra 0x00428340 Ship_UpdateVisualState, cloak-fade slice (all hulls,
// including the player). Advances the fade presentation state the gameplay
// visibility predicate reads (NovaTargeting_ShipAtCloakVisibilityThreshold):
// with the transition latch idle (0), an in-flight fade passively decays
// one unit per raw spaceflight call, time-adjusted against the original 21 ms
// frame floor; an armed latch (+1 fading in / -1 fading out) integrates
// latch * fade-rate * frame-tick-scale and clamps at the 32.0
// ceiling or the 0.0 floor (clearing the latch). The fade rate is 0.75
// ticks/frame, or 1.5 when the ship class carries Flags2 0x1
// (g_cloak_fade_rate_slow 0x0057530c vs g_cloak_fade_rate_fast 0x00575308).
// The original reads the ship-class swarming bit rather than the cloaking
// outfit's ModVal 0x0001; under kApplyOriginalBugFixes the fade gates on the
// device bit instead. A still-visible wreck (progress > 0) latches -2 so the
// fade continues to clear through the lower 8.0 visibility threshold. While
// 0 < progress < 32 the per-frame jitter offsets shared by every composite
// sprite layer are drawn from the session RNG (two
// NovaRandom_Range(2*trunc(progress/10)+1) rolls) and stored on the Ship for
// the renderer.
void NovaShip_TickCloakFadeState(GameState &state,
                                 Ship &ship,
                                 float elapsed_ticks);

// Ghidra 0x00428340 Ship_UpdateVisualState tail: maintain the per-ship cloak
// ability caches (+0xC91C screen reveal, +0xC91E radar reveal, +0xC920
// damage-deactivate). Inactive or out-of-system hulls are reset to the -1
// not-yet-computed sentinel; active hulls lazily populate each cache from
// Outfit_HasCloakScannerRevealForSurface (0x004652a0) and
// Outfit_HasCloakDamageDeactivateFlag (0x00464f60). Runs for every active hull
// in the per-ship visual pass and for the player in
// PlayerTick_InteractionCloakAndStatus.
void NovaShip_RefreshCloakAbilityCaches(GameState &state, Ship &ship);

} // namespace game
