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

// Resource four-byte type code for the ship-animation descriptor (sh\x8an).
constexpr std::uint32_t kShipVisualResourceType = 0x73688a6e;

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
  // AltImageID/AltMaskID (+0x0c/+0x0e): the alternate sprite sheet (bank/
  // unfold rows) appended after the base rotation rows in the same Sprite
  // (Ghidra Sprite_AssignSpriteSet with g_ship_sprite_alt[class]). > 0 when
  // the class has one.
  std::uint16_t alt_image_id = 0;
  std::uint16_t alt_mask_id = 0;
  // AltSetCount (+0x10): alternate rows appended after the base rotation row
  // (banking ships carry 2: bank-left, bank-right).
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

  // Per-turret-group weapon-exit (muzzle) offsets. EVN ships have up to four
  // turret groups, each with up to four quadrant barrels; a projectile is
  // offset off the ship centre to the barrel of its weapon's turret group,
  // alternating quadrant as the ship fires (Ghidra ShipClassDef
  // field_0xa42.. and Weapon_ApplyTurretSpreadVelocity 0x0046c5c0). The
  // loader (ShipClass_LoadShipClassVisualAndLaunchData) copies these shorts
  // from the sh\x8an descriptor at +0x48..+0xae.
  struct TurretGroupMuzzle {
    std::array<std::int16_t, 4> lateral{}; // field_0xa42 per quadrant
    std::array<std::int16_t, 4> forward{}; // field_0xa44 per quadrant
    std::array<std::int16_t, 4> drop{};    // field_0xa82 per quadrant
  };

  std::array<TurretGroupMuzzle, 4> turret_muzzles{};
  // Weapon-exit compress scales (ShipClassDef field_0xaa4/0xaa8), already
  // multiplied by the 0.01 compress factor (raw sh\x8an short * 0.01; for the
  // Shuttle 100*0.01 = 1.0 and 71*0.01 = 0.71, matching the loader defaults).
  float muzzle_scale_x = 0.0F;
  float muzzle_scale_y = 0.0F;
};

// Decodes one sh\x8an descriptor payload (Ghidra ShipClass_LoadShipClass-
// VisualAndLaunchData reads exactly these big-endian fields). Returns nullopt
// when the payload is too small for the header fields.
[[nodiscard]] std::optional<ShipVisualDescriptor>
DecodeShipVisualDescriptor(std::span<const std::byte> resource_data);

// Ghidra 0x00428340 Ship_UpdateVisualState, destruction slice (NPC hulls).
// Advances one destroyed ship's death presentation and runs the once-only
// destruction finale when the timer enters the (0, 2] tick window: blast
// damage to nearby hulls, the mission DESTRUCTION bookkeeping (quick-fail
// arm + goal_counter_a++ + target_ship_count--), the personality
// deactivation, and the hull deactivation. Reseeding when the timer has run
// out (DeathDelay) matches the original; hulls with DeathDelay 0 destruct
// immediately in the port (the original would linger forever - the port
// already spawns finale visuals immediately for those at the hit site).
// The debris-puff window (timer > 2) and audio are visual-only and owned by
// the SDL view (divergence logged in collision.cpp at the hit transition).
void NovaShip_TickDestroyedShipVisualState(GameState &state,
                                           Ship &ship,
                                           float elapsed_ticks);

// Ghidra 0x00428340 Ship_UpdateVisualState, destruction finale arm only.
// Runs the once-per-wreck bookkeeping and deactivates the hull; exported for
// the player-ship death path, which ticks its own presentation timer.
void NovaShip_RunShipDestructionFinale(GameState &state, Ship &ship);

} // namespace game
