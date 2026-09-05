#!/usr/bin/env python3
"""pict_decode.py — decode EV Nova PICT resources to RGBA.

Python port of src/pict_image.cpp (Resource_LoadPictAsImage 0x004b9050),
which itself mirrors the game's PICT walkers: the v2 opcode scanner
(Pict_ParseDirectBitsRect 0x004fd0a0, payload table DAT_00570344), the
classic 1-bit BitsRect fallback, and the row decoder FUN_004fcc00
(length-prefixed PackBits rows, raw narrow rows, 8-bit indexed / 1-bit /
RGB555 / 24-bit planar layouts).
"""

import struct


def _be16(d, o):
    return struct.unpack_from('>H', d, o)[0]


def _be16s(d, o):
    return struct.unpack_from('>h', d, o)[0]


# Ghidra DAT_00570344: payload byte count after opcode n; -1 = skip 2 bytes.
_OPCODE_PAYLOAD_SIZE = [
    0, 0, 8, 2,  2, 2, 4, 4,  2, 8, 8, 4,  4, 2, 4, 4,
    8, 1, 0, 0,  0, 2, 2, 0,  0, 0, 6, 6,  0, 6, 0, 6,
    8, 4, 6, 2,  -1, -1, -1, -1,
    0, 0, 0, 0,  -1, -1, -1, -1,
    8, 8, 8, 8,  8, 8, 8, 8,  0, 0, 0, 0,  0, 0, 0, 0,
    8, 8, 8, 8,  8, 8, 8, 8,  0, 0, 0, 0,  0, 0, 0, 0,
    8, 8, 8, 8,  8, 8, 8, 8,  0, 0, 0, 0,  0, 0, 0, 0,
    12, 12, 12, 12,  12, 12, 12, 12,  4, 4, 4, 4,  4, 4, 4, 4,
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
    0, 0, -1, -1,
    -1, -1, -1, -1,  -1, -1, -1, -1,  -1, -1, -1, -1,  -1, -1, -1, -1,
    2, 0,
]


class PictError(ValueError):
    pass


def _walk_to_bits_rect(data):
    """Ghidra 0x004fd0a0 opcode scan -> (payload_pos, opcode, width, height)."""
    if len(data) < 12:
        raise PictError("short PICT")
    version_pos = 10
    while version_pos < len(data) and data[version_pos] == 0:
        version_pos += 1
    if version_pos + 3 > len(data) or data[version_pos] != 0x11:
        raise PictError("no version marker")
    if data[version_pos + 1] != 2 or data[version_pos + 2] != 0xff:
        raise PictError("not a v2 PICT")

    width = _be16s(data, 8) - _be16s(data, 4)
    height = _be16s(data, 6) - _be16s(data, 2)

    pos = version_pos + 3
    while pos + 2 <= len(data):
        if pos & 1:
            pos += 1
            if pos + 2 > len(data):
                break
        opcode = _be16(data, pos)
        pos += 2
        if opcode < 0xa2:
            if opcode == 0x0001:
                if pos + 2 > len(data):
                    raise PictError("truncated 0x0001 block")
                block_size = _be16(data, pos)
                if block_size == 10 and pos + 10 <= len(data):
                    top, left, bottom, right = struct.unpack_from('>4H', data, pos + 2)
                    if bottom > top and right > left:
                        width, height = right - left, bottom - top
                if pos + block_size > len(data):
                    raise PictError("overlong 0x0001 block")
                pos += block_size
                continue
            if opcode in (0x0012, 0x0013, 0x0014):
                raise PictError("unsupported pattern opcode %#06x" % opcode)
            if opcode == 0x001b:
                pos += 6
                continue
            if 0x0070 <= opcode <= 0x0077:
                if pos + 2 > len(data):
                    raise PictError("truncated 0x007x block")
                pos += _be16(data, pos)
                continue
            if 0x0090 <= opcode <= 0x0091 or 0x0098 <= opcode <= 0x009b:
                if width <= 0 or height <= 0:
                    raise PictError("bad frame dimensions")
                return pos, opcode, width, height
            if opcode == 0x00a1:
                if pos + 4 > len(data):
                    raise PictError("truncated 0x00a1")
                pos += _be16(data, pos + 2) + 4
                continue
            skip = _OPCODE_PAYLOAD_SIZE[opcode]
            pos += 2 if skip < 0 else skip
            continue
        if opcode == 0x0c00:
            pos += 24
            continue
        if opcode == 0x0028:
            if pos + 5 > len(data):
                raise PictError("truncated 0x0028")
            pos += 5 + data[pos + 4]
            continue
        if opcode in (0x8200, 0x8201):
            raise PictError("QuickTime-compressed PICT unsupported")
        if opcode in (0x00ff, 0xffff):
            raise PictError("end of picture without image opcode")
        if 0x00d0 <= opcode <= 0x00fe or opcode >= 0x8100:
            if pos + 2 > len(data):
                raise PictError("truncated length-prefixed block")
            pos += _be16(data, pos)
            continue
        if 0x00ff < opcode < 0x8000:
            pos += (opcode >> 7) & 0xff
            continue
    raise PictError("no BitsRect opcode")


def _decode_packbits_row(encoded, output, unit_size):
    source = destination = 0
    while source < len(encoded) and destination < len(output):
        control = encoded[source]
        source += 1
        if control < 128:
            count = (control + 1) * unit_size
            if source + count > len(encoded) or destination + count > len(output):
                return False
            output[destination:destination + count] = encoded[source:source + count]
            source += count
            destination += count
        elif control != 128:
            count = (257 - control) * unit_size
            if source + unit_size > len(encoded) or destination + count > len(output):
                return False
            unit = encoded[source:source + unit_size]
            source += unit_size
            for _ in range(count // unit_size):
                output[destination:destination + unit_size] = unit
                destination += unit_size
    return source == len(encoded) and destination <= len(output)


def _decode_classic_bits_rect(data, payload):
    """1-bit classic BitsRect fallback (preferences arrow pictures)."""
    header = 28
    if payload + header > len(data):
        raise PictError("truncated classic BitsRect")
    row_bytes = _be16(data, payload) & 0x3fff
    top, left, bottom, right = struct.unpack_from('>4H', data, payload + 2)
    width, height = right - left, bottom - top
    if row_bytes == 0 or height <= 0 or width <= 0 or width > row_bytes * 8:
        raise PictError("bad classic BitsRect")
    source = payload + header
    if source + row_bytes * height > len(data):
        raise PictError("classic BitsRect data overruns resource")
    rgba = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            packed = data[source + y * row_bytes + x // 8]
            if packed & (1 << (7 - x % 8)):
                d = (y * width + x) * 4
                rgba[d:d + 4] = b'\xff\xff\xff\xff'
    return width, height, bytes(rgba)


def decode_pict(data):
    """Decode one PICT resource -> (width, height, rgba_bytes)."""
    try:
        payload, opcode, width, height = _walk_to_bits_rect(data)
    except PictError:
        # Version-1 / odd alignment: byte-scan for the 0x90 BitsRect opcode.
        for offset in range(10, len(data) - 1):
            if data[offset] == 0x90:
                return _decode_classic_bits_rect(data, offset + 1)
        raise

    direct = opcode in (0x009a, 0x009b)
    region_variant = opcode in (0x0091, 0x0099, 0x009b)
    pos = payload + (4 if direct else 0)
    if pos + 2 > len(data):
        raise PictError("truncated rowBytes field")
    row_bytes_raw = _be16(data, pos)
    row_bytes = row_bytes_raw & 0x7fff
    pos += 10  # rowBytes + bounds

    pixel_size, component_count = 1, 1
    if direct or (row_bytes_raw & 0x8000):
        if pos + 36 > len(data):
            raise PictError("truncated PixMap header")
        pixel_size = _be16(data, pos + 18)
        component_count = _be16(data, pos + 20)
        pos += 36

    if pixel_size == 8 and component_count == 1:
        fmt, unit_size, row_out = 'indexed8', 1, width
    elif pixel_size == 1 and component_count == 1:
        fmt, unit_size, row_out = 'mono', 1, row_bytes
    elif pixel_size == 16 and component_count == 3:
        fmt, unit_size, row_out = 'rgb555', 2, width * 2
    elif pixel_size == 32 and component_count == 3:
        fmt, unit_size, row_out = 'rgb24planar', 1, width * 3
    else:
        raise PictError("unsupported BitsRect layout (%d-bit, %d components)"
                        % (pixel_size, component_count))
    if fmt == 'mono' and width > row_bytes * 8:
        raise PictError("1-bit width %d exceeds row bytes %d" % (width, row_bytes))
    if row_bytes < row_out:
        raise PictError("rowBytes %d < expected %d" % (row_bytes, row_out))

    # Game's built-in 8-bit palette (DAT_005705cc): white entry 0, black rest.
    palette = bytearray(768)
    palette[0:3] = b'\xff\xff\xff'
    if not direct and (row_bytes_raw & 0x8000):
        if pos + 8 > len(data):
            raise PictError("truncated color table header")
        ct_flags = _be16(data, pos + 4)
        entry_count = _be16(data, pos + 6) + 1
        if pos + 8 + entry_count * 8 > len(data):
            raise PictError("color table overruns resource")
        pos += 8
        sequential = bool(ct_flags & 0x8000)
        for entry in range(entry_count):
            index = entry if sequential else _be16(data, pos)
            if index < 256:
                palette[index * 3] = data[pos + 2]
                palette[index * 3 + 1] = data[pos + 4]
                palette[index * 3 + 2] = data[pos + 6]
            pos += 8

    pos += 18  # source rect, destination rect, transfer mode
    if region_variant:
        if pos + 2 > len(data):
            raise PictError("truncated region size field")
        pos += _be16(data, pos)
    if pos > len(data):
        raise PictError("row data starts past the resource")

    effective_row_bytes = row_bytes if row_bytes != 0 else row_out
    raw_rows = effective_row_bytes < 8
    row = bytearray(row_bytes)
    rgba = bytearray(width * height * 4)
    for y in range(height):
        if raw_rows:
            if pos + effective_row_bytes > len(data):
                raise PictError("row %d: raw data truncated" % y)
            row[0:effective_row_bytes] = data[pos:pos + effective_row_bytes]
            pos += effective_row_bytes
        else:
            if pos + 2 > len(data):
                raise PictError("row %d: bad length" % y)
            if effective_row_bytes <= 0xfa:
                length = data[pos]
                pos += 1
            else:
                length = _be16(data, pos)
                pos += 2
            packed = data[pos:pos + length]
            if len(packed) < length:
                raise PictError("row %d: packed data truncated" % y)
            if not _decode_packbits_row(packed, row, unit_size):
                raise PictError("row %d: packbits decode failed" % y)
            pos += length
        for x in range(width):
            d = (y * width + x) * 4
            if fmt == 'indexed8':
                i = row[x] * 3
                rgba[d:d + 3] = palette[i:i + 3]
            elif fmt == 'mono':
                i = ((row[x // 8] >> (7 - x % 8)) & 1) * 3
                rgba[d:d + 3] = palette[i:i + 3]
            elif fmt == 'rgb555':
                p = (row[x * 2] << 8) | row[x * 2 + 1]
                rgba[d] = ((p >> 10) & 31) * 255 // 31
                rgba[d + 1] = ((p >> 5) & 31) * 255 // 31
                rgba[d + 2] = (p & 31) * 255 // 31
            else:  # rgb24planar
                rgba[d] = row[x]
                rgba[d + 1] = row[width + x]
                rgba[d + 2] = row[width * 2 + x]
            rgba[d + 3] = 255
    return width, height, bytes(rgba)
