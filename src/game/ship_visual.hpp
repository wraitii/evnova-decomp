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

#include <cstdint>
#include <optional>
#include <span>

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
  // FramesPer (+0x34): rotating frames for one full revolution (loader default
  // 36). Total frames in the sheet = base_set_count * frames_per_rotation.
  std::int16_t frames_per_rotation = 36;
  // Flags (+0x2e): the ship sprite behavior flags (bank/unfold/carry-animate).
  std::uint16_t sprite_behavior_flags = 0;
  // AnimDelay (+0x30): the loader stores this in ShipClassDef.combat_state_init_range.
  std::int16_t anim_delay = 0;
  // WeapDecay (+0x32): the weapon-glow fade rate; scaled into
  // ShipClassDef.weapon_glow_decay_rate.
  std::int16_t weapon_decay = 0;
};

// Decodes one sh\x8an descriptor payload (Ghidra ShipClass_LoadShipClass-
// VisualAndLaunchData reads exactly these big-endian fields). Returns nullopt
// when the payload is too small for the header fields.
[[nodiscard]] std::optional<ShipVisualDescriptor>
DecodeShipVisualDescriptor(std::span<const std::byte> resource_data);

} // namespace game
