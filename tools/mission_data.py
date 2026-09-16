#!/usr/bin/env python3
"""Inspect decoded EV Nova m\x95sn resources without starting the game.

Examples:
  tools/mission_data.py --name Tutorial
  tools/mission_data.py --id 251 --id 0x276 --json
  tools/mission_data.py --name 'Auroran' --full

The output reflects the resource fields consumed by ScenarioData::DecodeMission.
Locator values are shown verbatim: an exact stellar resource id is annotated
with its record name, but negative/special locator families remain numeric.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import struct
import sys
from typing import Any


MISSION_TYPE = 0x6D95736E  # m\x95sn
STELLAR_TYPE = 0x73709A62  # sp\x9ab
DESCRIPTION_TYPE = 0x64917363  # d\x91sc
MAP_RECORD_SIZE = 0x10A

# The named buffers copied into MisnActive by Mission_PopulateMissionSlotFromDef
# (Ghidra 0x0043f8c0); see src/game/mission.cpp.
SCRIPT_OFFSETS = {
    "on_accept": 0x15B,    # Bible OnAccept
    "on_refuse": 0x25A,    # Bible OnRefuse
    "on_success": 0x359,   # Bible OnSuccess
    "on_failure": 0x458,   # Bible OnFailure
    "on_abort": 0x557,     # Bible OnAbort
    "on_ship_done": 0x660, # Bible OnShipDone
}


def be_i16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">h", data, offset)[0]


def be_i32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">i", data, offset)[0]


def c_string(data: bytes, offset: int) -> str:
    return data[offset:].split(b"\0", 1)[0].decode("mac_roman", "replace")


def parse_archive(path: Path) -> list[tuple[int, int, str, bytes]]:
    """Return (type, resource-id, record-name, payload) records from BRGR."""
    data = path.read_bytes()
    if data[:4] != b"BRGR" or len(data) < 0x18:
        raise ValueError("not a BRGR archive")
    entry_count = struct.unpack_from("<I", data, 0x14)[0]
    entries = [struct.unpack_from("<II", data, 0x18 + index * 12)
               for index in range(entry_count)]

    for region_offset, region_size in entries:
        if region_size < 8:
            continue
        type_dir_offset, type_count = struct.unpack_from(">II", data, region_offset)
        if not (0 < type_count <= 500 and type_dir_offset <= region_size and
                type_dir_offset + type_count * 12 <= region_size):
            continue
        types: list[tuple[int, int, int]] = []
        for index in range(type_count):
            type_code, records_offset, record_count = struct.unpack_from(
                ">III", data, region_offset + type_dir_offset + index * 12
            )
            if (records_offset > region_size or
                    record_count > (region_size - records_offset) // MAP_RECORD_SIZE):
                break
            types.append((type_code, records_offset, record_count))
        else:
            records: list[tuple[int, int, str, bytes]] = []
            for type_code, records_offset, record_count in types:
                for index in range(record_count):
                    record_offset = region_offset + records_offset + index * MAP_RECORD_SIZE
                    entry_index = struct.unpack_from(">I", data, record_offset)[0]
                    resource_id = struct.unpack_from(">H", data, record_offset + 8)[0]
                    if entry_index == 0 or entry_index > len(entries):
                        continue
                    name = c_string(data[record_offset:record_offset + MAP_RECORD_SIZE], 10)
                    payload_offset, payload_size = entries[entry_index - 1]
                    records.append((type_code, resource_id, name,
                                    data[payload_offset:payload_offset + payload_size]))
            return records
    raise ValueError("no parseable resource.map")


def locator(value: int, stellars: dict[int, str]) -> dict[str, Any]:
    result: dict[str, Any] = {"raw": value}
    if value in stellars:
        result["stellar"] = stellars[value]
    return result


def decode_mission(resource_id: int, name: str, data: bytes,
                   stellars: dict[int, str], descriptions: dict[int, str]) -> dict[str, Any]:
    if len(data) < 0x7A2:
        raise ValueError(f"m\x95sn {resource_id} is truncated ({len(data)} bytes)")
    text_ids = [be_i16(data, 0x34 + index * 2) for index in range(6)]
    description_ids = {
        "brief": text_ids[0], "quick_brief": text_ids[1],
        "load_cargo": text_ids[2], "dump_cargo": text_ids[3],
        "success": text_ids[4], "failure": text_ids[5],
        "ship_done": be_i16(data, 0x44),
    }
    result = {
        "id": resource_id,
        "name": name.split(";", 1)[0].rstrip(),
        "subtitle": name.partition(";")[2],
        "link_system_filter": be_i16(data, 0x00),
        "availability": {
            "location": be_i16(data, 0x04),
            "record": be_i16(data, 0x06),
            "rating": be_i16(data, 0x08),
            "random_percent": be_i16(data, 0x0A),
            "expression": c_string(data, 0x5C),
        },
        "travel_stellar": locator(be_i16(data, 0x0C), stellars),
        "return_stellar": locator(be_i16(data, 0x0E), stellars),
        "cargo": {
            "type": be_i16(data, 0x10),
            "tons": be_i16(data, 0x12),
            "pickup_mode": be_i16(data, 0x14),
            "drop_off_mode": be_i16(data, 0x16),
            "scan_mask": be_i16(data, 0x18),
        },
        "payment": be_i32(data, 0x1C),
        "mission_ship": {
            "count": be_i16(data, 0x20),
            "system": locator(be_i16(data, 0x22), stellars),
            "personality": be_i16(data, 0x24),
            "goal": be_i16(data, 0x26),
            "behavior": be_i16(data, 0x28),
            "start": be_i16(data, 0x2C),
        },
        "text_ids": description_ids,
        "time_limit_days": be_i16(data, 0x40),
        "can_abort": be_i16(data, 0x42) != 0,
        "flags": {"primary": f"0x{struct.unpack_from('>H', data, 0x50)[0]:04x}",
                  "secondary": f"0x{struct.unpack_from('>H', data, 0x52)[0]:04x}"},
        "ship_restriction": be_i16(data, 0x5A),
        "list_priority": be_i16(data, 0x7A0),
        "scripts": {key: c_string(data, offset) for key, offset in SCRIPT_OFFSETS.items()},
    }
    if descriptions:
        result["texts"] = {
            key: descriptions[value] for key, value in description_ids.items()
            if value >= 0 and value in descriptions
        }
    return result


def print_summary(mission: dict[str, Any], full: bool) -> None:
    avail = mission["availability"]
    travel = mission["travel_stellar"]
    returned = mission["return_stellar"]
    def format_locator(value: dict[str, Any]) -> str:
        return f"{value['raw']} ({value['stellar']})" if "stellar" in value else str(value["raw"])
    print(f"{mission['id']:>4}  {mission['name']}")
    print(f"      offer: loc={avail['location']} expr={avail['expression']!r} "
          f"random={avail['random_percent']}%")
    print(f"      route: travel={format_locator(travel)} return={format_locator(returned)}")
    if full:
        print(json.dumps(mission, indent=2, ensure_ascii=False))
    else:
        scripts = ", ".join(f"{key}={value!r}" for key, value in mission["scripts"].items() if value)
        if scripts:
            print(f"      scripts: {scripts}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--id", action="append", type=lambda value: int(value, 0),
                        help="resource id in decimal or 0x-prefixed hexadecimal; repeatable")
    parser.add_argument("--name", help="case-insensitive regular expression matched against record names")
    parser.add_argument("--full", action="store_true", help="show every decoded field")
    parser.add_argument("--text", action="store_true",
                        help="resolve referenced d\x91sc text resources as well")
    parser.add_argument("--json", action="store_true", help="emit the selected records as JSON")
    parser.add_argument("--data-dir", type=Path,
                        default=Path(__file__).resolve().parents[1] / "EV Nova" / "Nova Files",
                        help="directory containing Nova Data *.rez archives")
    args = parser.parse_args()
    if args.name:
        try:
            name_pattern = re.compile(args.name, re.IGNORECASE)
        except re.error as error:
            parser.error(f"invalid --name regular expression: {error}")
    else:
        name_pattern = None

    records: list[tuple[int, int, str, bytes]] = []
    for archive in sorted(args.data_dir.glob("Nova Data *.rez")):
        records.extend(parse_archive(archive))
    if not records:
        parser.error(f"no records found under {args.data_dir}")
    stellars = {resource_id: name.split(";", 1)[0].rstrip()
                for type_code, resource_id, name, _ in records if type_code == STELLAR_TYPE}
    descriptions = ({resource_id: c_string(data, 0)
                     for type_code, resource_id, _, data in records
                     if type_code == DESCRIPTION_TYPE}
                    if args.text else {})
    requested_ids = set(args.id or [])
    missions = [decode_mission(resource_id, name, data, stellars, descriptions)
                for type_code, resource_id, name, data in records
                if type_code == MISSION_TYPE
                and (not requested_ids or resource_id in requested_ids)
                and (name_pattern is None or name_pattern.search(name))]
    missions.sort(key=lambda mission: mission["id"])
    if args.json:
        print(json.dumps(missions, indent=2, ensure_ascii=False))
    else:
        for mission in missions:
            print_summary(mission, args.full)
    if not missions:
        print("No matching missions.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
