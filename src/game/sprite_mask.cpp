#include "sprite_mask.hpp"

#include "../brgr_archive.hpp"
#include "../rle_sprite_sheet.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

namespace game {
namespace {

[[nodiscard]] bool ResolveFrameMasks(const RleSpriteSheet &sheet,
                                     std::vector<SpriteMask> &out) {
  out.clear();
  if (sheet.width <= 0 || sheet.height <= 0) {
    return false;
  }
  out.reserve(sheet.frames.size());
  for (const RleSpriteFrame &frame : sheet.frames) {
    out.push_back(
        SpriteMask_FromRgba(frame.rgba_pixels, sheet.width, sheet.height));
  }
  return true;
}

} // namespace

SpriteMask SpriteMask_FromRgba(std::span<const std::uint8_t> rgba_pixels,
                               int width,
                               int height,
                               std::uint8_t alpha_threshold) {
  SpriteMask mask;
  if (width <= 0 || height <= 0) {
    return mask;
  }
  const auto pixel_count =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (rgba_pixels.size() < pixel_count * 4U) {
    return mask;
  }
  mask.width = width;
  mask.height = height;
  mask.opaque.resize(pixel_count, 0);
  for (std::size_t i = 0; i < pixel_count; ++i) {
    if (rgba_pixels[i * 4U + 3U] >= alpha_threshold) {
      mask.opaque[i] = 1;
    }
  }
  return mask;
}

bool SpriteMask_TestOverlap(const SpriteMask &a,
                            float a_world_x,
                            float a_world_y,
                            float a_anchor_x,
                            float a_anchor_y,
                            const SpriteMask &b,
                            float b_world_x,
                            float b_world_y,
                            float b_anchor_x,
                            float b_anchor_y) {
  if (a.Empty() || b.Empty()) {
    return false;
  }
  // Frame top-left in the shared world/screen space (Sprite_SetPositionFrom-
  // CurrentFrameAnchor subtracts the frame-local anchor from the world point).
  const int a_left = static_cast<int>(std::lround(a_world_x - a_anchor_x));
  const int a_top = static_cast<int>(std::lround(a_world_y - a_anchor_y));
  const int b_left = static_cast<int>(std::lround(b_world_x - b_anchor_x));
  const int b_top = static_cast<int>(std::lround(b_world_y - b_anchor_y));

  const int ix0 = std::max(a_left, b_left);
  const int iy0 = std::max(a_top, b_top);
  const int ix1 = std::min(a_left + a.width, b_left + b.width);
  const int iy1 = std::min(a_top + a.height, b_top + b.height);
  if (ix0 >= ix1 || iy0 >= iy1) {
    return false;
  }

  for (int y = iy0; y < iy1; ++y) {
    for (int x = ix0; x < ix1; ++x) {
      if (a.OpaqueAt(x - a_left, y - a_top) &&
          b.OpaqueAt(x - b_left, y - b_top)) {
        return true;
      }
    }
  }
  return false;
}

bool SpriteMask_TestBoundingCircleOverlap(int a_width,
                                          int a_height,
                                          float a_world_x,
                                          float a_world_y,
                                          int b_width,
                                          int b_height,
                                          float b_world_x,
                                          float b_world_y) {
  (void)a_width;
  (void)b_width;
  // Ghidra (0x00475be0) derives the half-span from the frame height
  // (bottom - top)/2 and treats the top-left + half-height as the centre.
  const int a_radius = std::max(0, a_height) / 2;
  const int b_radius = std::max(0, b_height) / 2;
  const int radius = a_radius + b_radius;
  const int dx = static_cast<int>(std::lround(a_world_x - b_world_x));
  const int dy = static_cast<int>(std::lround(a_world_y - b_world_y));
  return dx * dx + dy * dy < radius * radius;
}

bool CollisionMask_TestContact(const CollisionMaskBinding &a,
                               float a_world_x,
                               float a_world_y,
                               int a_circle_radius,
                               const CollisionMaskBinding &b,
                               float b_world_x,
                               float b_world_y,
                               int b_circle_radius,
                               bool allow_pixel_mask) {
  if (allow_pixel_mask && a.HasMask() && b.HasMask()) {
    return SpriteMask_TestOverlap(*a.mask,
                                  a_world_x,
                                  a_world_y,
                                  a.anchor_x,
                                  a.anchor_y,
                                  *b.mask,
                                  b_world_x,
                                  b_world_y,
                                  b.anchor_x,
                                  b.anchor_y);
  }
  const int dx = static_cast<int>(std::lround(a_world_x - b_world_x));
  const int dy = static_cast<int>(std::lround(a_world_y - b_world_y));
  const int radius =
      std::max(0, a_circle_radius) + std::max(0, b_circle_radius);
  return dx * dx + dy * dy <= radius * radius;
}

// ---------------------------------------------------------------------------
// Non-SDL frame-mask cache
// ---------------------------------------------------------------------------
struct SpriteMaskStore::Impl {
  struct Entry {
    std::vector<SpriteMask> frames;
  };

  mutable std::vector<std::optional<Entry>> spin_cache;
  mutable std::vector<std::optional<Entry>> sheet_cache;

  // Ensures the cache slot for `id` is populated (empty Entry on failure) and
  // returns it; pointers into `frames` stay valid because entries are only
  // assigned once and vector moves preserve the inner buffer.
  template <typename LoadFn>
  [[nodiscard]] const Entry *Ensure(std::vector<std::optional<Entry>> &cache,
                                    std::uint16_t id,
                                    LoadFn &&load) const {
    const auto index = static_cast<std::size_t>(id);
    if (cache.size() <= index) {
      cache.resize(index + 1);
    }
    if (!cache[index].has_value()) {
      auto loaded = load();
      cache[index] = loaded.has_value() ? std::move(*loaded) : Entry{};
    }
    return &*cache[index];
  }

  [[nodiscard]] static const SpriteMask *Lookup(const Entry *entry, int frame) {
    if (entry == nullptr || entry->frames.empty()) {
      return nullptr;
    }
    const int clamped =
        std::clamp(frame, 0, static_cast<int>(entry->frames.size()) - 1);
    return &entry->frames[static_cast<std::size_t>(clamped)];
  }

  [[nodiscard]] std::optional<Entry> LoadSheet(std::uint16_t sheet_id) const {
    const auto sheet_data =
        NovaResource_Load(kResourceTypeRleSheet16, sheet_id);
    if (!sheet_data) {
      return std::nullopt;
    }
    const auto sheet = RleSpriteSheet_Decode16(*sheet_data);
    if (!sheet) {
      return std::nullopt;
    }
    Entry entry;
    if (!ResolveFrameMasks(*sheet, entry.frames)) {
      return std::nullopt;
    }
    return entry;
  }

  [[nodiscard]] std::optional<Entry> LoadSpin(std::uint16_t spin_id) const {
    const auto spin_data = NovaResource_Load(kResourceTypeSprites, spin_id);
    if (!spin_data) {
      return std::nullopt;
    }
    const auto def = NovaSpriteDefinition_Parse(*spin_data);
    if (!def || def->tile_width <= 0 || def->tile_height <= 0) {
      return std::nullopt;
    }
    const auto sheet_data =
        NovaResource_Load(kResourceTypeRleSheet16, def->sprites_resource_id);
    if (!sheet_data) {
      return std::nullopt;
    }
    const auto sheet = RleSpriteSheet_Decode16(*sheet_data);
    if (!sheet || sheet->width != def->tile_width ||
        sheet->height != def->tile_height) {
      return std::nullopt;
    }
    Entry entry;
    if (!ResolveFrameMasks(*sheet, entry.frames)) {
      return std::nullopt;
    }
    return entry;
  }
};

SpriteMaskStore::SpriteMaskStore() : impl_(std::make_unique<Impl>()) {}

SpriteMaskStore::~SpriteMaskStore() = default;
SpriteMaskStore::SpriteMaskStore(SpriteMaskStore &&) noexcept = default;
SpriteMaskStore &
SpriteMaskStore::operator=(SpriteMaskStore &&) noexcept = default;

const SpriteMask *SpriteMaskStore::Spin(std::uint16_t spin_id,
                                        int frame) const {
  const Impl::Entry *entry =
      impl_->Ensure(impl_->spin_cache, spin_id, [this, spin_id]() {
        return impl_->LoadSpin(spin_id);
      });
  return Impl::Lookup(entry, frame);
}

const SpriteMask *SpriteMaskStore::Sheet(std::uint16_t sheet_id,
                                         int frame) const {
  const Impl::Entry *entry =
      impl_->Ensure(impl_->sheet_cache, sheet_id, [this, sheet_id]() {
        return impl_->LoadSheet(sheet_id);
      });
  return Impl::Lookup(entry, frame);
}

int SpriteMaskStore::SpinFrameCount(std::uint16_t spin_id) const {
  const Impl::Entry *entry =
      impl_->Ensure(impl_->spin_cache, spin_id, [this, spin_id]() {
        return impl_->LoadSpin(spin_id);
      });
  return entry == nullptr ? 0 : static_cast<int>(entry->frames.size());
}

int SpriteMaskStore::SheetFrameCount(std::uint16_t sheet_id) const {
  const Impl::Entry *entry =
      impl_->Ensure(impl_->sheet_cache, sheet_id, [this, sheet_id]() {
        return impl_->LoadSheet(sheet_id);
      });
  return entry == nullptr ? 0 : static_cast<int>(entry->frames.size());
}

} // namespace game
