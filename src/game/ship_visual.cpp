#include "ship_visual.hpp"

#include <cstdint>

namespace game {
namespace {

[[nodiscard]] std::uint16_t ReadBe16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])
                                    << 8U |
                                    std::to_integer<std::uint8_t>(
                                        bytes[offset + 1]));
}

[[nodiscard]] std::int16_t ReadBeI16(std::span<const std::byte> bytes,
                                     std::size_t offset) {
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
  d.base_x_size = ReadBe16(resource_data, 0x06); // BaseXSize
  d.base_y_size = ReadBe16(resource_data, 0x08); // BaseYSize
  d.base_transparency = ReadBeI16(resource_data, 0x0a); // BaseTransp
  d.sprite_behavior_flags = ReadBe16(resource_data, 0x2e); // Flags
  d.anim_delay = ReadBeI16(resource_data, 0x30);           // AnimDelay
  d.weapon_decay = ReadBeI16(resource_data, 0x32);         // WeapDecay
  d.frames_per_rotation = ReadBeI16(resource_data, 0x34);  // FramesPer
  if (d.frames_per_rotation == 0) {
    d.frames_per_rotation = 36;
  }
  return d;
}

} // namespace game
