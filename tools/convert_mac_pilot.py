#!/usr/bin/env python3
"""Convert classic Mac EV Nova pilot files to Windows-style .plt files.

Accepts a MacBinary-wrapped pilot (type Np\x95L) or a raw resource fork. The
classic Mac save stores its two persistence blocks as Np\x95L resources 128
and 129; the Windows save frames the corresponding blocks with little-endian
sizes. StuffIt archives must be extracted before invoking this tool.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
from pathlib import Path


PILOT_RESOURCE_TYPE = b"Np\x95L"
BLOCK1_SIZE = 0xE952
MAC_BLOCK1_SIZE = 0xE9B2
BLOCK2_SIZE = 0x66FE
SIMPLE_CRYPT_KEY = 0xB36A210F
SIMPLE_CRYPT_MODIFIER = 0xDEADBEEF


def simple_crypt(data: bytes) -> bytes:
    """Andrew Welch's symmetric SimpleCrypt transform."""
    transformed = bytearray(data)
    key = SIMPLE_CRYPT_KEY
    for offset in range(0, len(transformed) - len(transformed) % 4, 4):
        chunk = int.from_bytes(transformed[offset : offset + 4], "big") ^ key
        transformed[offset : offset + 4] = chunk.to_bytes(4, "big")
        key = ((key + SIMPLE_CRYPT_MODIFIER) & 0xFFFFFFFF) ^ SIMPLE_CRYPT_MODIFIER
    for offset in range(len(transformed) - len(transformed) % 4, len(transformed)):
        transformed[offset] ^= key >> 24
        key = (key << 8) & 0xFFFFFFFF
    return bytes(transformed)


def convert_primary_block(mac_block: bytes) -> bytes:
    """Remove Mac mission padding while preserving its big-endian payload."""
    if len(mac_block) != MAC_BLOCK1_SIZE:
        raise ValueError(f"unexpected Mac primary block size: {len(mac_block):#x}")
    decrypted = simple_crypt(mac_block)
    windows = bytearray(decrypted[:0x295E])
    cursor = 0x295E
    for _ in range(16):
        mission = bytearray()
        mission.extend(decrypted[cursor : cursor + 0x20])
        cursor += 0x22  # Mac +0x20 short is absent on Windows.
        mission.extend(decrypted[cursor : cursor + 0x13])
        cursor += 0x14  # Mac +0x35 byte is absent on Windows.
        mission.extend(decrypted[cursor : cursor + 0x8B1])
        cursor += 0x8B4  # Mac's final three pad bytes are absent on Windows.
        windows.extend(mission)
    windows.extend(decrypted[cursor:])
    if len(windows) != BLOCK1_SIZE:
        raise ValueError(f"converted primary block has size {len(windows):#x}")
    return simple_crypt(windows)


def convert_secondary_block(mac_block: bytes) -> bytes:
    if len(mac_block) != BLOCK2_SIZE:
        raise ValueError(f"unexpected Mac secondary block size: {len(mac_block):#x}")
    return mac_block


def macbinary_resource_fork(data: bytes) -> tuple[str, bytes] | None:
    if len(data) < 128 or data[0] != 0 or not 1 <= data[1] <= 63:
        return None
    name = data[2 : 2 + data[1]].decode("mac_roman")
    data_length, resource_length = struct.unpack_from(">II", data, 83)
    resource_offset = 128 + (data_length + 127) // 128 * 128
    resource_end = resource_offset + resource_length
    if resource_length == 0 or resource_end > len(data):
        raise ValueError("truncated or absent MacBinary resource fork")
    return name, data[resource_offset:resource_end]


def appledouble_resource_fork(data: bytes) -> bytes | None:
    if len(data) < 26 or data[:4] != b"\x00\x05\x16\x07":
        return None
    entry_count = struct.unpack_from(">H", data, 24)[0]
    for entry_index in range(entry_count):
        descriptor = 26 + entry_index * 12
        if descriptor + 12 > len(data):
            raise ValueError("AppleDouble entry list is truncated")
        entry_id, offset, length = struct.unpack_from(">III", data, descriptor)
        if offset + length > len(data):
            raise ValueError("AppleDouble entry points outside the file")
        if entry_id == 2:  # Resource fork.
            return data[offset : offset + length]
    raise ValueError("AppleDouble file has no resource-fork entry")


def resource_entries(
    resource_fork: bytes,
) -> tuple[dict[tuple[bytes, int], bytes], dict[tuple[bytes, int], bytes]]:
    if len(resource_fork) < 16:
        raise ValueError("resource fork header is truncated")
    data_offset, map_offset, data_length, map_length = struct.unpack_from(
        ">IIII", resource_fork
    )
    if (
        data_offset + data_length > len(resource_fork)
        or map_offset + map_length > len(resource_fork)
        or map_length < 30
    ):
        raise ValueError("resource fork header points outside the file")

    type_list = map_offset + struct.unpack_from(">H", resource_fork, map_offset + 24)[0]
    name_list = map_offset + struct.unpack_from(">H", resource_fork, map_offset + 26)[0]
    type_count = struct.unpack_from(">H", resource_fork, type_list)[0] + 1
    entries: dict[tuple[bytes, int], bytes] = {}
    names: dict[tuple[bytes, int], bytes] = {}
    for type_index in range(type_count):
        type_entry = type_list + 2 + type_index * 8
        if type_entry + 8 > len(resource_fork):
            raise ValueError("resource type list is truncated")
        resource_type = resource_fork[type_entry : type_entry + 4]
        resource_count = struct.unpack_from(">H", resource_fork, type_entry + 4)[0] + 1
        references = type_list + struct.unpack_from(">H", resource_fork, type_entry + 6)[0]
        for resource_index in range(resource_count):
            reference = references + resource_index * 12
            if reference + 12 > len(resource_fork):
                raise ValueError("resource reference list is truncated")
            resource_id = struct.unpack_from(">h", resource_fork, reference)[0]
            name_offset = struct.unpack_from(">h", resource_fork, reference + 2)[0]
            relative_offset = int.from_bytes(resource_fork[reference + 5 : reference + 8], "big")
            length_offset = data_offset + relative_offset
            if length_offset + 4 > len(resource_fork):
                raise ValueError("resource data offset is outside the file")
            length = struct.unpack_from(">I", resource_fork, length_offset)[0]
            start = length_offset + 4
            if start + length > len(resource_fork):
                raise ValueError("resource data is truncated")
            key = (resource_type, resource_id)
            entries[key] = resource_fork[start : start + length]
            if name_offset >= 0:
                name = name_list + name_offset
                if name >= len(resource_fork):
                    raise ValueError("resource name offset is outside the file")
                name_length = resource_fork[name]
                name_start = name + 1
                name_end = name_start + name_length
                if name_end > len(resource_fork):
                    raise ValueError("resource name is truncated")
                names[key] = resource_fork[name_start:name_end]
    return entries, names


def native_resource_fork(path: Path) -> bytes:
    try:
        return os.getxattr(path, "com.apple.ResourceFork")
    except AttributeError:
        # Some python.org/Homebrew builds omit os.getxattr on macOS.
        result = subprocess.run(
            ["/usr/bin/xattr", "-px", "com.apple.ResourceFork", path],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            return bytes.fromhex(result.stdout)
    except OSError:
        pass
    return b""


def convert(source: Path, output_dir: Path) -> Path:
    source_data = source.read_bytes()
    native_resource = native_resource_fork(source)
    apple_resource = appledouble_resource_fork(source_data)
    if native_resource:
        pilot_name = source.name
        resource_fork = native_resource
    elif apple_resource is not None:
        pilot_name = source.name.removesuffix(".rsrc")
        resource_fork = apple_resource
    else:
        wrapped = macbinary_resource_fork(source_data)
        if wrapped is not None:
            pilot_name, resource_fork = wrapped
        else:
            pilot_name = source.stem
            resource_fork = source_data

    entries, names = resource_entries(resource_fork)
    block1 = entries.get((PILOT_RESOURCE_TYPE, 128))
    block2 = entries.get((PILOT_RESOURCE_TYPE, 129))
    if block1 is None or block2 is None:
        raise ValueError("not an EV Nova pilot resource fork (Np\\x95L 128/129 missing)")
    if len(block1) != MAC_BLOCK1_SIZE or len(block2) != BLOCK2_SIZE:
        raise ValueError(
            f"unexpected pilot block sizes: {len(block1):#x}, {len(block2):#x}"
        )

    # Mac resource 128 has six padding bytes in every mission record. Windows
    # omits those bytes, shifting all later fields by 16*6 = 96 bytes.
    # The ship name is the MacRoman resource name of block 129; the Windows
    # format stores the same value as its trailing C string.
    ship_name = names.get((PILOT_RESOURCE_TYPE, 129), b"")
    converted_block1 = convert_primary_block(block1)
    converted_block2 = convert_secondary_block(block2)
    output = (
        struct.pack("<I", BLOCK1_SIZE)
        + converted_block1
        + struct.pack("<I", BLOCK2_SIZE)
        + converted_block2
        + ship_name
        + b"\0"
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    destination = output_dir / f"{pilot_name}.plt"
    destination.write_bytes(output)
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", type=Path, nargs="+")
    parser.add_argument("--output-dir", type=Path, default=Path("docs/assets/pilots"))
    args = parser.parse_args()

    failed = False
    for source in args.sources:
        try:
            destination = convert(source, args.output_dir)
            print(f"{source} -> {destination}")
        except (OSError, UnicodeError, ValueError, struct.error) as error:
            failed = True
            print(f"{source}: {error}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
