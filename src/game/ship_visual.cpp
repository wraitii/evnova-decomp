#include "ship_visual.hpp"

#include <cstdint>

namespace game {
namespace {

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) << 8U |
      std::to_integer<std::uint8_t>(bytes[offset + 1]));
}

[[nodiscard]] std::int16_t ReadBeI16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  if (offset + 1 >= bytes.size()) {
    return 0; // descriptor too short for this field; treat as zero
  }
  return static_cast<std::int16_t>(ReadBe16(bytes, offset));
}

constexpr std::size_t kMinDescriptorSize = 0x36;

} // namespace

std::optional<ShipVisualDescriptor>
DecodeShipVisualDescriptor(std::span<const std::byte> resource_data) {
  if (resource_data.size() < kMinDescriptorSize) {
    return std::nullopt;
  }
  ShipVisualDescriptor d;
  d.base_image_id = ReadBe16(resource_data, 0x00);   // BaseImageID
  d.base_mask_id = ReadBe16(resource_data, 0x02);    // BaseMaskID
  d.base_set_count = ReadBeI16(resource_data, 0x04); // BaseSetCount (>=1)
  if (d.base_set_count < 1) {
    d.base_set_count = 1;
  }
  d.base_x_size = ReadBe16(resource_data, 0x06);           // BaseXSize
  d.base_y_size = ReadBe16(resource_data, 0x08);           // BaseYSize
  d.base_transparency = ReadBeI16(resource_data, 0x0a);    // BaseTransp
  d.sprite_behavior_flags = ReadBe16(resource_data, 0x2e); // Flags
  d.anim_delay = ReadBeI16(resource_data, 0x30);           // AnimDelay
  d.weapon_decay = ReadBeI16(resource_data, 0x32);         // WeapDecay
  // Engine-glow layer (+0x16 image / +0x18 mask / +0x1a x / +0x1c y), read as
  // big-endian shorts like the base fields. Negative/zero id = no glow layer.
  d.engine_glow_image_id = ReadBeI16(resource_data, 0x16); // GlowImageID
  d.engine_glow_mask_id = ReadBeI16(resource_data, 0x18);  // GlowMaskID
  d.engine_glow_x_size = ReadBe16(resource_data, 0x1a);    // GlowXSize
  d.engine_glow_y_size = ReadBe16(resource_data, 0x1c);    // GlowYSize
  d.frames_per_rotation = ReadBeI16(resource_data, 0x34);  // FramesPer
  if (d.frames_per_rotation == 0) {
    d.frames_per_rotation = 36;
  }
  // Per-turret-group weapon-exit (muzzle) offsets. The loader
  // (ShipClass_LoadShipClassVisualAndLaunchData 0x004b4ee0) copies these from
  // the sh\x8an payload into ShipClassDef field_0xa42..; Weapon_ApplyTurret-
  // SpreadVelocity (0x0046c5c0) reads them per turret group x quadrant to
  // offset a projectile's muzzle. Layout (group g, quadrant q):
    //   lateral[g][q] = sh\x8an 0x48 + 16g + 2q  (field_0xa42)
    //   forward[g][q] = sh\x8an 0x50 + 16g + 2q  (field_0xa44)
    //   drop[g][q]    = sh\x8an 0x90 + 16g + 2q  (field_0xa82)
  for (std::size_t g = 0; g < d.turret_muzzles.size(); ++g) {
    auto &m = d.turret_muzzles[g];
    for (std::size_t q = 0; q < 4; ++q) {
      m.lateral[q] = ReadBeI16(resource_data, 0x48 + g * 16 + q * 2);
      m.forward[q] = ReadBeI16(resource_data, 0x50 + g * 16 + q * 2);
      m.drop[q] = ReadBeI16(resource_data, 0x90 + g * 16 + q * 2);
    }
  }
  // Compress scales (field_0xaa4/0xaa8); the loader multiplies the raw short
  // by the 0.01 weapon-exit compress scale.
  d.muzzle_scale_x = static_cast<float>(ReadBeI16(resource_data, 0x88)) * 0.01F;
  d.muzzle_scale_y = static_cast<float>(ReadBeI16(resource_data, 0x8a)) * 0.01F;
  return d;
}

} // namespace game
