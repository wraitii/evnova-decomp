#pragma once

#include <cstddef>
#include <optional>
#include <span>

#include "pict_image.hpp"

// Decodes a Mac `ppat` (pixel pattern) resource into a plain RGBA image.
//
// The stellar radar's interference static tiles one of ten preloaded `ppat`
// resources 128..137 (NovaUi_DrawStellarRadarPanel 0x0045d600 ->
// DrawContext_TileImageInRect 0x004bbdc0). Resource_LoadPixPat (0x004bbd50)
// looks the resource up and hands its payload to the PixPat decoder
// (thunk_Resource_LoadPixPatAsImage 0x004fdf40, whose body is the late-linked
// library routine Resource_LoadPixPatAsImage at 0x0087293e); the decoder
// returns an indexed pixel buffer plus a 256-entry RGB palette, which the
// blitter consumes.
//
// This clean-room decoder reproduces that result. The payload is:
//   +0x00  u16 patType (must be 1)
//   +0x02  u32 offset to the PixMap (read big-endian, sign-extended to 16 bits)
//   +0x06  u32 offset to the packed pixel raster (same)
// The PixMap at that offset: rowBytes (+0x04, top bits 0xc000 == 0x8000),
// bounds top/left/bottom/right (+0x06/+0x08/+0x0a/+0x0c), two zero resolution
// words (+0x0e/+0x10), pixelSize in bits at +0x20 (1/2/4/8), and the inline
// ColorTable offset at +0x2a. Pixels are MSB-first indices with a masked
// rowBytes stride; the ColorTable is [seed u32][flags u16][count u16] then
// `count` entries of [index u16][r u16][g u16][b u16] (high bytes at +3/+5/+7).
// A pixel index with no ColorTable entry stays black, matching the original's
// zero-initialized palette (the original also reads only `count` entries, so
// the final entry -- observed black in every shipped ppat -- is skipped).
[[nodiscard]] std::optional<PictImage>
Resource_LoadPixPatAsImage(std::span<const std::byte> ppat_data);
