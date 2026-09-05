#!/usr/bin/env python3
"""ship_spin_extract.py — extract EV Nova ship spin art to PNG (docs/assets/ships).

Walks every shän (0x73688a6e) ship-animation descriptor in the Nova Ships *.rez
archives, follows its rlëD (0x726c9144) image/mask sheet ids, decodes the 16-bit
RLE sheets with the same algorithm as src/rle_sprite_sheet.cpp
(RleSpriteSheet_Decode16, mirror of Ghidra's sprite loader), and writes one
folder per ship class under docs/assets/ships/:

    docs/assets/ships/<shän id> - <class name>/
        info.txt          decoded descriptor fields + source ids
        base.png          tiled spin/rotation frames (frame 0 = pointing up)
        base_mask.png     base mask sheet as grayscale (if present)
        alt.png / alt_mask.png / glow*.png / light*.png /
        weapon*.png / shield*.png   (only when the shän id is non-zero)

Pure-Python, no engine dependency; PIL is used only for PNG encoding.

Usage:  python3 tools/ship_spin_extract.py [output_dir]
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rez_extract as rz  # noqa: E402

import pict_decode
from PIL import Image  # noqa: E402

SHAN = b'sh\x8an'   # 0x73688a6e ship animation descriptor
RLRD = b'rl\x91D'   # 0x726c9144 16-bit RLE sprite sheet
PICT = b'PICT'
DESC = b'd\x91sc'   # 0x64917363 description (text + detail-window PICT id)

# shän big-endian field offsets (Ghidra 0x004b4ee0
# ShipClass_LoadShipClassVisualAndLaunchData; see docs/ship_sprite_rendering_path.md).
ROLES = [
    ("base",   0x00),  # image id; mask +0x02
    ("alt",    0x0c),  # mask +0x0e
    ("glow",   0x16),  # mask +0x18
    ("light",  0x1e),  # mask +0x20
    ("weapon", 0x26),  # mask +0x28
    ("shield", 0x40),  # mask +0x42
]


def be(d, o, n):
    return int.from_bytes(d[o:o + n], 'big')


def decode_rlrd(data):
    """Decode one rlëD 16-bit RLE sheet -> (width, height, [RGBA frames]).

    Mirrors RleSpriteSheet_Decode16 (src/rle_sprite_sheet.cpp) op-for-op:
    0x10-byte header [w][h][depth=16][frame_count], then per frame a stream of
    32-bit big-endian commands (opcode<<24 | count):
      0 end-of-frame, 1 next row, 2 raw 16-bit pixels (4-byte padded),
      3 skip (transparent run), 4 4-byte alternating pixel pattern.
    """
    if len(data) < 0x10:
        raise ValueError("short rlD header")
    width, height, depth, frame_count = be(data, 0, 2), be(data, 2, 2), be(data, 4, 2), be(data, 8, 2)
    if depth != 16 or width == 0 or height == 0 or frame_count == 0:
        raise ValueError("unsupported rlD header (depth=%d)" % depth)
    frames = []
    src = 0x10
    for _ in range(frame_count):
        pixels = bytearray(width * height * 4)  # RGBA, starts transparent
        row = -1
        row_pos = 0
        row_end = None
        while src + 4 <= len(data):
            command = be(data, src, 4)
            src += 4
            opcode, count = command >> 24, command & 0xFFFFFF
            if opcode == 0:
                break
            if opcode == 1:
                row += 1
                row_pos = 0
                row_end = src + count
                continue
            if opcode == 2:
                padded = (count + 3) & ~3
                for off in range(0, count, 2):
                    pixel = be(data, src + off, 2)
                    _store_rgb555(pixels, row, row_pos + off, width, pixel)
                src += padded
            elif opcode == 3:
                pass  # transparent run: destination pixels stay 0
            elif opcode == 4:
                for off in range(0, count, 2):
                    pixel = be(data, src + (off & 2), 2)
                    _store_rgb555(pixels, row, row_pos + off, width, pixel)
                src += 4
            else:
                raise ValueError("bad rlD opcode %#x" % opcode)
            row_pos += count
        frames.append(bytes(pixels))
    return width, height, frames


def _store_rgb555(pixels, row, byte_pos, width, pixel):
    idx = (row * width + byte_pos // 2) * 4
    pixels[idx] = ((pixel >> 10) & 31) * 255 // 31
    pixels[idx + 1] = ((pixel >> 5) & 31) * 255 // 31
    pixels[idx + 2] = (pixel & 31) * 255 // 31
    pixels[idx + 3] = 255


def tile_png(width, height, frames, columns=12):
    """Tile decoded frames into one grid image (frame 0 top-left)."""
    n = len(frames)
    cols = min(n, columns) if n > columns else n
    rows = (n + cols - 1) // cols
    img = Image.new("RGBA", (width * cols, height * rows), (0, 0, 0, 0))
    for i, px in enumerate(frames):
        frame = Image.frombytes("RGBA", (width, height), px)
        img.paste(frame, ((i % cols) * width, (i // cols) * height))
    return img


def mask_png(width, height, frames, columns=12):
    """Render mask sheet frames as grayscale (mask pixel value = intensity)."""
    gray = []
    for px in frames:
        g = bytearray(width * height)
        for p in range(width * height):
            r, g_, b = px[p * 4], px[p * 4 + 1], px[p * 4 + 2]
            g[p] = max(r, g_, b)  # mask pixels are grey in RGB555; keep magnitude
        gray.append(bytes(g))
    return tile_gray(width, height, gray, columns)


def tile_gray(width, height, frames, columns=12):
    n = len(frames)
    cols = min(n, columns) if n > columns else n
    rows = (n + cols - 1) // cols
    img = Image.new("L", (width * cols, height * rows), 0)
    for i, px in enumerate(frames):
        frame = Image.frombytes("L", (width, height), px)
        img.paste(frame, ((i % cols) * width, (i // cols) * height))
    return img


# shän Flags (+0x2e) bits (EVN Bible). The first four are mutually exclusive
# and decide what the extra frame sets in the base sheet are used for.
FLAG_NAMES = [
    (0x0001, "banking (sets: level / banking left / banking right)"),
    (0x0002, "animated parts (cycle on land / takeoff / hyperspace)"),
    (0x0004, "set 2 shown while carrying a KeyCarried ship"),
    (0x0008, "extra frames cycled like alt sprites (AnimDelay)"),
    (0x0010, "stop animations when disabled"),
    (0x0020, "hide alt sprites when disabled"),
    (0x0040, "hide running lights when disabled"),
    (0x0080, "unfolds while firing weapons"),
    (0x0100, "UpCompress/DnCompress off-axis skew correction"),
]


def describe_flags(flags, set_count):
    bits = [text for mask, text in FLAG_NAMES if flags & mask]
    out = ", ".join(bits) if bits else "none"
    if set_count > 1 and not flags & 0x000F:
        out += " (set count > 1 but no set-semantics flag set)"
    return out


def sanitize(name):
    name = name.split(';')[0].strip()  # resource names carry ';subtitle' suffixes
    name = re.sub(r'[^\w .+-]+', '_', name).strip(' ._')
    return name or "unnamed"


def collect():
    """Return (shan, rlrld, picts, descs) as {id: (archive, payload, name)}."""
    shan, rlrld, picts, descs = {}, {}, {}, {}
    for path in rz.default_paths():
        try:
            archive = rz.Archive(path)
        except (ValueError, OSError):
            continue
        for tc, idx, rid, name in archive.map_records:
            if tc == SHAN:
                shan.setdefault(rid, (archive.name, archive.payload(idx), name))
            elif tc == RLRD:
                rlrld.setdefault(rid, (archive.name, archive.payload(idx), name))
            elif tc == PICT:
                picts.setdefault(rid, (archive.name, archive.payload(idx), name))
            elif tc == DESC:
                descs.setdefault(rid, (archive.name, archive.payload(idx), name))
    return shan, rlrld, picts, descs


def desc_graphic_pict_id(payload):
    """The dësc selection-dialog Graphic PICT id (brgr_archive.cpp
    NovaResource_LoadDescription): NUL-terminated text, then a 2-byte BE
    picture id; ids < 0x80 mean "no custom picture"."""
    nul = payload.find(b'\x00')
    if nul < 0 or nul + 3 > len(payload):
        return None
    return int.from_bytes(payload[nul + 1:nul + 3], 'big')


README_TEXT = """\
# Ship spin assets (extracted)

PNG exports of the in-game ship spin art, one folder per ship class, generated
by `tools/ship_spin_extract.py` (re-run that script to regenerate):

```
python3 tools/ship_spin_extract.py
```

Folder names are `<shän resource id> - <class name>`; the `shän` id equals the
ship class id (classes are `0x80`-based, so `128 - Shuttle` is class 0).

Per folder:

- `base.png` — the rotation ("spin") sheet from the `shän` BaseImageID
  `rlëD` resource, all frames tiled 12 per row. Frame 0 points **up**. A
  rotation set holds `frames_per_rotation` frames; the base sheet contains
  `BaseSetCount` sets whose meaning depends on the descriptor's Flags bits
  (first four are mutually exclusive, per the EVN Bible):

  | flag | extra sets used for |
  |------|---------------------|
  | 0x0001 | **banking** in heavy turns: set 1 = level flight, set 2 = banking left, set 3 = banking right |
  | 0x0002 | animated ship parts, cycled on landing / takeoff / hyperspace |
  | 0x0004 | set 2 shown while carrying a KeyCarried ship |
  | 0x0008 | extra frames cycled in sequence like alt sprites (AnimDelay) |

  e.g. the Shuttle (36 rotations × 3 sets, flags 0x0041) is one level set plus
  left/right bank sets; with no flag the sets are just more rotation frames.
- `glow.png`, `light.png`, `alt.png`, `weapon.png`, `shield.png` — the other
  sprite layers referenced by the descriptor, when present.
- `hud.png` — target info pict (PICT `3000 + class index`, 128×64) shown in
  the HUD target window.
- `shipyard.png` — large shipyard / ship-comm portrait (PICT
  `5000 + class index`, 200×200).
  Only the first class of each identical-looking series carries its own
  portraits; higher classes with the same `shän` BaseImageID reuse the clone
  source's PICT (the `info.txt` line notes this fallback).
- `shipinfo.png` — the large 600×400 ship-detail picture shown by the
  shipyard Info window's custom-picture variant (DLOG 0x3fb). Its PICT id
  comes from the ship's `dësc` resource (`13000 + class index`) Graphic field;
  ships whose dësc has no picture id (`< 0x80`) or no dësc at all use the
  small pictureless window and have no `shipinfo.png`.
- `*_mask.png` — mask sheets, only if the mask `rlëD` actually exists (most
  mask ids in the shipped scenario are vestigial: when the 16-bit `rlëD`
  image sheet is present the engine loads it as one multi-frame resource and
  never opens the separate mask).
- `info.txt` — descriptor fields (frames per rotation, set counts, flags) and
  the source `rlëD` ids + archives.

Sheets are decoded from the `Nova Ships *.rez` archives' 16-bit RLE `rlëD`
resources with the same algorithm as `src/rle_sprite_sheet.cpp`
(`RleSpriteSheet_Decode16`); colours are RGB555 scaled to 8 bits per channel.
"""


def main():
    out_root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 'assets', 'ships')
    out_root = os.path.abspath(out_root)
    shan, rlrld, picts, descs = collect()
    print(f"{len(shan)} shan descriptors, {len(rlrld)} rlD sheets, "
          f"{len(picts)} PICTs, {len(descs)} desc resources")

    # Clone-source map (Ghidra NovaData_LoadAllShipClassVisualAndLaunchData
    # 0x004aeda0 / scenario_data.cpp): the lowest class id with a given shän
    # BaseImageID owns the target (PICT 3000+index) and shipyard (PICT
    # 5000+index) portraits; identical-looking higher classes reuse them.
    first_class_by_base_image = {}
    clone_source = {}
    for rid in sorted(shan):
        data = shan[rid][1]
        if len(data) >= 2:
            base_image = be(data, 0x00, 2)
            owner = first_class_by_base_image.setdefault(base_image, rid - 0x80)
            clone_source[rid] = owner
        else:
            clone_source[rid] = rid - 0x80
    os.makedirs(out_root, exist_ok=True)
    with open(os.path.join(out_root, "README.md"), "w") as fh:
        fh.write(README_TEXT)

    extracted = failed = 0
    for rid in sorted(shan):
        archive_name, data, name = shan[rid]
        if len(data) < 0x44:
            print(f"  #{rid}: shan too short ({len(data)} bytes)", file=sys.stderr)
            continue
        folder = os.path.join(out_root, "%d - %s" % (rid, sanitize(name)))
        lines = [
            "shän id: %d (ship class id %d)" % (rid, rid),
            "class name: %s" % name.split(';')[0].strip(),
            "source archive: %s" % archive_name,
            "frames per rotation: %d" % (be(data, 0x34, 2) or 36),
            "base set count: %d" % max(1, be(data, 0x04, 2)),
            "base transparency: %d" % be(data, 0x0a, 2),
            "sprite behavior flags: 0x%04x (%s)"
            % (be(data, 0x2e, 2),
               describe_flags(be(data, 0x2e, 2), max(1, be(data, 0x04, 2)))),
        ]
        folder_ok = True
        for role, off in ROLES:
            # ids are signed 16-bit; the loader skips roles with id <= 0
            # (0xFFFF = -1 = "none" is the common marker).
            image_id = be(data, off, 2)
            if image_id >= 0x8000:
                continue
            mask_id = be(data, off + 2, 2)
            if mask_id >= 0x8000:
                mask_id = 0
            sheet = rlrld.get(image_id)
            if sheet is None:
                print(f"  #{rid} {name}: missing rlD {image_id} for {role}", file=sys.stderr)
                folder_ok = False
                continue
            try:
                width, height, frames = decode_rlrd(sheet[1])
            except ValueError as e:
                print(f"  #{rid} {name}: rlD {image_id} ({role}): {e}", file=sys.stderr)
                folder_ok = False
                continue
            os.makedirs(folder, exist_ok=True)
            tile_png(width, height, frames).save(
                os.path.join(folder, f"{role}.png"))
            lines.append(f"{role}: rlD {image_id}  {width}x{height}  "
                         f"{len(frames)} frames  ({sheet[0]})")
            # Separate mask sheets are effectively vestigial: when the 16-bit
            # rlD image sheet exists the engine always takes the
            # Sprite_CreateFromMultiFrameResource path and never loads the
            # mask id, so a missing mask resource is normal, not an error.
            mask_sheet = rlrld.get(mask_id)
            if mask_sheet:
                try:
                    mw, mh, mframes = decode_rlrd(mask_sheet[1])
                    mask_png(mw, mh, mframes).save(
                        os.path.join(folder, f"{role}_mask.png"))
                    lines.append(f"{role} mask: rlD {mask_id}  {mw}x{mh}  "
                                 f"{len(mframes)} frames")
                except ValueError as e:
                    print(f"  #{rid} {name}: mask rlD {mask_id}: {e}", file=sys.stderr)
            extracted += 1
        # Target info pict (HUD target window, 128x64) and shipyard/ship-comm
        # portrait (200x200); own id first, clone source as fallback.
        index = rid - 0x80
        owner = clone_source[rid]
        for role, base_id in (("hud", 3000), ("shipyard", 5000)):
            pict_id = base_id + index
            note = ""
            if pict_id not in picts:
                pict_id = base_id + owner
                note = "  (clone fallback from class %d)" % (owner + 0x80)
            sheet = picts.get(pict_id)
            if sheet is None:
                lines.append(f"{role}: PICT {base_id + index} absent (no fallback)")
                continue
            try:
                w, h, rgba = pict_decode.decode_pict(sheet[1])
            except pict_decode.PictError as e:
                print(f"  #{rid} {name}: PICT {pict_id} ({role}): {e}", file=sys.stderr)
                folder_ok = False
                continue
            Image.frombytes("RGBA", (w, h), rgba).save(
                os.path.join(folder, f"{role}.png"))
            lines.append(f"{role}: PICT {pict_id}  {w}x{h}  ({sheet[0]}){note}")
        # Ship detail-window picture (NovaUi_RunShipyardDetailWindow
        # 0x004956a0): the ship's dësc (id = 13000 + class index) carries the
        # Graphic PICT id; ids < 0x80 mean the pictureless DLOG 0x3ed window.
        desc = descs.get(13000 + index)
        if desc is None:
            lines.append("ship info: dësc %d absent (no detail picture)"
                         % (13000 + index))
        else:
            graphic_id = desc_graphic_pict_id(desc[1])
            sheet = picts.get(graphic_id) if graphic_id and graphic_id >= 0x80 else None
            if sheet is None:
                lines.append("ship info: none (dësc %d graphic id %s)"
                             % (13000 + index, graphic_id))
            else:
                try:
                    w, h, rgba = pict_decode.decode_pict(sheet[1])
                except pict_decode.PictError as e:
                    print(f"  #{rid} {name}: PICT {graphic_id} (ship info): {e}",
                          file=sys.stderr)
                    folder_ok = False
                else:
                    Image.frombytes("RGBA", (w, h), rgba).save(
                        os.path.join(folder, "shipinfo.png"))
                    lines.append(f"ship info: dësc {13000 + index} -> PICT "
                                 f"{graphic_id}  {w}x{h}  ({sheet[0]})")
        if folder_ok and extracted:
            with open(os.path.join(folder, "info.txt"), "w") as fh:
                fh.write("\n".join(lines) + "\n")
        else:
            failed += 1
    print(f"wrote {extracted} sheets under {out_root}"
          + (f" ({failed} ships had failures)" if failed else ""))


if __name__ == '__main__':
    main()
