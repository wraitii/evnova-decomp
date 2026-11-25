#!/usr/bin/env python3
"""ppat_extract.py — extract Mac `ppat` (pixel pattern) resources from EV Nova .rez files.

Writes each ppat as raw `<archive>_ppat_<id>.ppat` (the resource payload, usable
in ResForge) and, when Pillow is installed (and --no-png is not given), a decoded
`.png` beside it. Decoding mirrors src/pixpat_image.cpp (Resource_LoadPixPatAsImage
0x0087293e); the ColorTable is read with the full QuickDraw count (ctSize + 1).

Usage:
    tools/ppat_extract.py OUT_DIR [file.rez ...] [--no-png] [--id 128]
Defaults to every .rez under "EV Nova/".
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rez_extract import Archive, default_paths  # noqa: E402


def _short_offset(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def decode_ppat(d):
    """Return (width, height, rgba bytes) or raise ValueError."""
    if len(d) < 0x2C or struct.unpack_from('>H', d, 0)[0] != 1:
        raise ValueError("not a version-1 pixel pattern")
    pm = _short_offset(struct.unpack_from('>I', d, 2)[0])
    pix = _short_offset(struct.unpack_from('>I', d, 6)[0])
    if pm <= 0 or pix <= 0 or pm + 0x2E > len(d):
        raise ValueError("bad PixMap/pixel offsets")
    rb_word, top, left, bottom, right = struct.unpack_from('>Hhhhh', d, pm + 4)
    if rb_word & 0xC000 != 0x8000:
        raise ValueError("unsupported rowBytes flags %#06x" % rb_word)
    w, h, row_bytes = right - left, bottom - top, rb_word & 0x3FFF
    bpp = struct.unpack_from('>H', d, pm + 0x20)[0]
    if bpp not in (1, 2, 4, 8) or not (0 < w <= 0x1000 and 0 < h <= 0x1000) or row_bytes == 0:
        raise ValueError("unsupported geometry %dx%d @%dbpp" % (w, h, bpp))
    ct = _short_offset(struct.unpack_from('>I', d, pm + 0x2A)[0])
    if ct <= 0 or ct + 8 > len(d):
        raise ValueError("missing ColorTable")
    ct_size = struct.unpack_from('>H', d, ct + 6)[0]
    pal = {}
    for i in range(ct_size + 1):
        e = ct + 8 + i * 8
        if e + 8 > len(d):
            break
        idx = struct.unpack_from('>H', d, e)[0]
        pal[idx] = (d[e + 2], d[e + 4], d[e + 6])  # high byte of each 16-bit channel
    per = 8 // bpp
    mask = (1 << bpp) - 1
    out = bytearray()
    for y in range(h):
        base = pix + y * row_bytes
        for x in range(w):
            s = base + x // per
            if s >= len(d):
                raise ValueError("pixel raster out of range")
            idx = (d[s] >> ((8 - bpp) - (x % per) * bpp)) & mask
            out += bytes(pal.get(idx, (0, 0, 0))) + b'\xff'
    return w, h, bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out_dir')
    ap.add_argument('rez', nargs='*')
    ap.add_argument('--no-png', action='store_true')
    ap.add_argument('--id', type=int, help="only this resource id")
    args = ap.parse_args()

    Image = None
    if not args.no_png:
        try:
            from PIL import Image
        except ImportError:
            print("Pillow not installed; writing raw .ppat only", file=sys.stderr)

    os.makedirs(args.out_dir, exist_ok=True)
    count = 0
    for path in args.rez or default_paths():
        try:
            arc = Archive(path)
        except (ValueError, OSError) as e:
            print("# %s: %s" % (os.path.basename(path), e), file=sys.stderr)
            continue
        stem = os.path.splitext(arc.name)[0].replace(' ', '_')
        for tc, idx, rid, name in arc.map_records:
            if tc != b'ppat' or (args.id is not None and rid != args.id):
                continue
            data = arc.payload(idx)
            base = os.path.join(args.out_dir, "%s_ppat_%d" % (stem, rid))
            with open(base + '.ppat', 'wb') as f:
                f.write(data)
            note = ''
            if Image:
                try:
                    w, h, rgba = decode_ppat(data)
                    Image.frombytes('RGBA', (w, h), rgba).save(base + '.png')
                    note = " %dx%d" % (w, h)
                except ValueError as e:
                    note = " (png skipped: %s)" % e
            print("%s id=%d %r %d bytes%s" % (arc.name, rid, name, len(data), note))
            count += 1
    print("extracted %d ppat resources to %s" % (count, args.out_dir))


if __name__ == '__main__':
    main()
