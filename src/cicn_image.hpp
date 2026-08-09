#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "pict_image.hpp"

// Decodes a Mac `cicn` (color icon) resource into a plain RGBA image.
//
// EV Nova builds its in-flight corner-bracket reticles and the target cursor
// from cicn resources (the ship reticle's 16-frame set is cicn 10008-10023,
// the travel reticle's 8-frame set cicn 10000-10007, described in
// FUN_004ad960; the cursor is spin 650's cicn). Ghidra Resource_LoadCicn
// (0x004bbb80) -> FUN_004d2bd0 parses the extended "color icon" header and
// unpacks a per-pixel colour-index raster plus a 1-bit mask; the sprite-layer
// builder then paints the palette-mapped pixels over a white rect and derives
// the frame's alpha mask from the same mask bitmap (FUN_00476800 /
// FUN_00479070).
//
// This clean-room decoder reproduces that result: width/height come from
// pixelBounds (+0x0c-+0x08, +0x0a-+0x06); the mask bitmap is a 1-bit-per-pixel
// raster of `maskRowBytes` (BE16 at +0x36) per row beginning at payload
// +0x52; the palette (colour-index -> RGB, entry = [index u16][r][g][b], 8
// bytes each) and the colour-index raster follow (the palette count header
// sits at payload +0x52 + ((BE16 at +0x44) + (BE16 at +0x36)) * height).
// Pixels whose mask bit is clear are transparent; palette index 0 maps to
// opaque white (the game fills the frame white where the mask is set), so it
// is composited as white rather than left black.
[[nodiscard]] std::optional<PictImage>
Resource_LoadCicnAsImage(std::span<const std::byte> cicn_data);
