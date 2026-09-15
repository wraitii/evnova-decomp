#!/usr/bin/env python3
"""Convert classic Mac EV Nova pilot files to Windows-style .plt files.

Accepts a MacBinary-wrapped pilot (type Np\x95L) or a raw resource fork. The
classic Mac save stores its two persistence blocks as Np\x95L resources 128
and 129; the Windows save frames the corresponding blocks with little-endian
sizes. StuffIt archives must be extracted before invoking this tool.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


PILOT_RESOURCE_TYPE = b"Np\x95L"
BLOCK1_SIZE = 0xE952
BLOCK2_SIZE = 0x66FE


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


def resource_entries(resource_fork: bytes) -> dict[tuple[bytes, int], bytes]:
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
    type_count = struct.unpack_from(">H", resource_fork, type_list)[0] + 1
    entries: dict[tuple[bytes, int], bytes] = {}
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
            relative_offset = int.from_bytes(resource_fork[reference + 5 : reference + 8], "big")
            length_offset = data_offset + relative_offset
            if length_offset + 4 > len(resource_fork):
                raise ValueError("resource data offset is outside the file")
            length = struct.unpack_from(">I", resource_fork, length_offset)[0]
            start = length_offset + 4
            if start + length > len(resource_fork):
                raise ValueError("resource data is truncated")
            entries[(resource_type, resource_id)] = resource_fork[start : start + length]
    return entries


def convert(source: Path, output_dir: Path) -> Path:
    source_data = source.read_bytes()
    apple_resource = appledouble_resource_fork(source_data)
    if apple_resource is not None:
        pilot_name = source.name.removesuffix(".rsrc")
        resource_fork = apple_resource
    else:
        wrapped = macbinary_resource_fork(source_data)
        if wrapped is not None:
            pilot_name, resource_fork = wrapped
        else:
            pilot_name = source.stem
            resource_fork = source_data

    entries = resource_entries(resource_fork)
    block1 = entries.get((PILOT_RESOURCE_TYPE, 128))
    block2 = entries.get((PILOT_RESOURCE_TYPE, 129))
    if block1 is None or block2 is None:
        raise ValueError("not an EV Nova pilot resource fork (Np\\x95L 128/129 missing)")
    if len(block1) < BLOCK1_SIZE or len(block2) != BLOCK2_SIZE:
        raise ValueError(
            f"unexpected pilot block sizes: {len(block1):#x}, {len(block2):#x}"
        )

    # Mac resource 128 has a 96-byte platform tail after the Windows block.
    # The ship-name trailer is not separately represented in the Mac fork;
    # leave it empty rather than inventing a value.
    output = (
        struct.pack("<I", BLOCK1_SIZE)
        + block1[:BLOCK1_SIZE]
        + struct.pack("<I", BLOCK2_SIZE)
        + block2
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
