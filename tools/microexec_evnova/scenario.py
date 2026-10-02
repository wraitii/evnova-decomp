"""microexec data plugin: EV Nova's shipped scenario data (Nova Data *.rez).

The original copies resource payloads into fixed-size global tables
(`g_outfit_defs`, ...), reordering fields on the way. Our port's decoder for
each family names the payload offset of every field, so a Ghidra field that the
port decodes under the same name can be resolved to a payload offset and
measured across every shipped record. That is measured data for the
`scenario` evidence level: what the stock Nova data holds, not how often a
running game reads it.

Loaded by microexec through `plugins` in tools/microexec_evnova/microexec.toml.
"""

from __future__ import annotations

import importlib.util
import json
import re
import statistics
from collections import Counter
from functools import lru_cache
from pathlib import Path
from typing import Any, Callable

from project import DataPlugin, EvidenceKind, Project

# Ghidra table global -> (resource type, family name, port decoder in src/game/scenario_data.cpp)
FAMILIES: dict[str, tuple[bytes, str, str]] = {
    "g_ship_class_defs": (b"sh\x95p", "ships", "DecodeShip"),
    "g_outfit_defs": (b"o\x9ftf", "outfits", "DecodeOutfit"),
    "g_weapon_defs": (b"w\x91ap", "weapons", "DecodeWeapon"),
    "g_stellar_defs": (b"sp\x9ab", "stellars", "DecodeStellar"),
    "g_government_defs": (b"g\x9avt", "governments", "DecodeGovernment"),
}
SOURCE = Path("src/game/scenario_data.cpp")
TOP_VALUES = 8
MAX_RESOURCES = 16
_READ = re.compile(r"ReadBe(?P<signed>I?)(?P<bits>8|16|32)\(\s*\w+\s*,\s*(?P<offset>0x[0-9a-fA-F]+|\d+)\s*\)")


def _rez_module(repo: Path) -> Any:
    spec = importlib.util.spec_from_file_location("rez_extract", repo / "tools" / "rez_extract.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def archive_paths(repo: Path) -> list[Path]:
    return sorted((repo / "EV Nova" / "Nova Files").glob("Nova Data *.rez"))


@lru_cache(maxsize=1)
def _archives(repo: Path) -> tuple[Any, ...]:
    module = _rez_module(repo)
    return tuple(module.Archive(p) for p in archive_paths(repo))


def records(repo: Path, type_code: bytes) -> list[tuple[int, bytes]]:
    """(resource id, payload) of every shipped record of a type, in archive order."""
    out = []
    for archive in _archives(repo):
        for tc, entry, rid, _ in archive.map_records:
            if tc == type_code:
                base, length = archive.entries[entry - 1]
                out.append((rid, archive.data[base:base + length]))
    return out


def field_stats(repo: Path, type_code: bytes, offset: int, width: int, signed: bool,
                transform: Callable[[int], int] | None = None) -> dict[str, Any] | None:
    """Distribution of one big-endian integer field across the shipped records."""
    if width not in (1, 2, 4) or offset < 0:
        raise ValueError("width must be 1, 2 or 4 and offset non-negative")
    values = [int.from_bytes(p[offset:offset + width], "big", signed=signed)
              for _, p in records(repo, type_code) if len(p) >= offset + width]
    if not values:
        return None
    if transform:
        values = [transform(v) for v in values]
    values.sort()
    counts = Counter(values)
    n = len(values)
    return {"records": n, "min": values[0], "p10": values[n // 10], "median": values[n // 2],
            "p90": values[min(n - 1, n * 9 // 10)], "max": values[-1], "mean": round(statistics.fmean(values), 2),
            "zero_fraction": round(counts[0] / n, 3), "distinct": len(counts),
            "top_values": [[v, c] for v, c in counts.most_common(TOP_VALUES)]}


def decoder_source(repo: Path, name: str) -> str | None:
    """Text of a port decoder function, from its signature line to its closing brace."""
    path = repo / SOURCE
    if not path.exists():
        return None
    lines = path.read_text(errors="replace").splitlines()
    start = next((i for i, line in enumerate(lines) if re.search(rf"\b{name}\(", line) and not line.startswith(" ")), None)
    if start is None:
        return None
    begin = start
    while begin > 0 and lines[begin - 1].startswith("//"):
        begin -= 1  # the layout comment above the decoder
    end = next((i for i in range(start + 1, len(lines)) if lines[i] == "}"), len(lines) - 1)
    return "\n".join(lines[begin:end + 1])


def loader_for(key: str, repo: Path) -> tuple[str, str] | None:
    """(family, decoder text) for a slot key rooted in a scenario table global."""
    family = FAMILIES.get(key.split("[", 1)[0].split(".", 1)[0])
    if family is None:
        return None
    source = decoder_source(repo, family[2])
    return (family[1], source) if source else None


def slot_hint(repo: Path, key: str) -> dict[str, Any] | None:
    """Measured distribution for `g_table[*].field` when the port decodes the same field name.

    Returns None unless the field name resolves to one integer read in the
    port's decoder for that table (a renamed or derived field has no hint;
    the model can still ask for `scenario_loader` and `scenario_stats`).
    """
    match = re.fullmatch(r"(g_\w+)\[\*\]\.(\w+)", key)
    if not match or match[1] not in FAMILIES:
        return None
    type_code, family, decoder = FAMILIES[match[1]]
    source = decoder_source(repo, decoder)
    if not source:
        return None
    field = match[2]
    hits = [line for line in source.splitlines() if re.search(rf"\.{re.escape(field)}(\[\d+\])?\s*=", line)]
    reads = [found[0] for line in hits if len(found := list(_READ.finditer(line))) == 1]
    # Ghidra 0x004bd3c0: negative Holds clears +0xa40 and is negated in the signed-short capacity.
    # The second assignment is normalization, not another payload field.
    normalized_holds = match[1] == "g_ship_class_defs" and field == "cargo_holds"
    if (len(hits) != 1 and not normalized_holds) or len(reads) != 1:
        return None
    read = reads[0]
    width = int(read["bits"]) // 8
    offset = int(read["offset"], 0)
    transform = (lambda v: ((abs(v) + 32768) % 65536) - 32768) if normalized_holds else None
    stats = field_stats(repo, type_code, offset, width, bool(read["signed"]), transform)
    if stats is None:
        return None
    return ({"source": "shipped Nova Data payloads (scenario records, not runtime frequency)",
            "family": family, "payload_offset": f"0x{offset:x}", "width_bytes": width,
            "signed": bool(read["signed"]), "port_line": next(line.strip() for line in hits if _READ.search(line))}
            | ({"normalization": "signed-short absolute capacity (negative Holds forbids mass expansions)"}
               if normalized_holds else {}) | stats)


def family_overview(repo: Path, keys: list[str]) -> str:
    """One line per scenario family the slots are rooted in: type code and record count."""
    lines = []
    for global_name in sorted({k.split("[", 1)[0].split(".", 1)[0] for k in keys} & FAMILIES.keys()):
        type_code, family, decoder = FAMILIES[global_name]
        n = len(records(repo, type_code))
        lines.append(f"- `{global_name}` holds the {family} family: resource type {type_code.hex()}, "
                     f"{n} shipped records (ids from 128, stored zero-based); port decoder `{decoder}`.")
    return "\n".join(lines)


def _type_code(request: dict[str, Any]) -> bytes:
    type_name = request["type"]
    if not isinstance(type_name, str):
        raise ValueError("resource type must be a four-character code or eight hex digits")
    type_code = bytes.fromhex(type_name) if re.fullmatch(r"[0-9a-fA-F]{8}", type_name) else type_name.encode("latin1")
    if len(type_code) != 4:
        raise ValueError("resource type must contain exactly four bytes")
    return type_code


class ScenarioPlugin(DataPlugin):
    name = "scenario"
    ranking = "measured scenario data (`scenario_observed`, `scenario_stats`)"
    caveat = "Measured scenario data says what shipped records contain, not how often a running game reads them."
    observed_guide = ("Use `scenario_observed` (the distribution of that field across the shipped records) whenever "
                      "it is present: stay inside it unless the code shows the slot holds something else (a sum, a "
                      "running counter, a per-ship copy).")
    evidence_notes = ("Resource windows are limited to 256 bytes; omit id to list up to sixteen resources; high-byte "
                      "type codes use eight hex digits (the scenario families section lists them). Scenario bytes "
                      "establish resource contents, not mid-game frequencies.")

    def __init__(self, repo: Path):
        self.repo = repo

    def slot_hint(self, key: str) -> dict[str, Any] | None:
        return slot_hint(self.repo, key)

    def section(self, keys: list[str]) -> tuple[str, str]:
        return "Scenario families", family_overview(self.repo, keys) or "(no slot is rooted in a scenario table)"

    def evidence_kinds(self) -> dict[str, EvidenceKind]:
        return {
            "scenario_loader": EvidenceKind(
                '  {"kind":"scenario_loader", "family":"outfits"}           // our port\'s decoder: payload offsets per field',
                self._loader),
            "scenario_stats": EvidenceKind(
                '  {"kind":"scenario_stats", "type":"6f9f7466", "offset":14, "width":4, "signed":true}   '
                '// distribution over all shipped records',
                self._stats),
            "scenario_resource": EvidenceKind(
                '  {"kind":"scenario_resource", "type":"679a7674", "id":128, "offset":0, "size":64}',
                self._resource),
        }

    def _loader(self, request: dict[str, Any]) -> str:
        family = str(request.get("family", ""))
        for name, (_, fam, decoder) in FAMILIES.items():
            if family in (fam, name, decoder) and (text := decoder_source(self.repo, decoder)):
                return (f"Source: our C++ port's decoder for {fam} ({SOURCE}); each line names a "
                        f"big-endian payload offset. Weak evidence for names, verified only where the "
                        f"comment says so.\n{text}")
        raise ValueError("family must be one of " + ", ".join(v[1] for v in FAMILIES.values()))

    def _stats(self, request: dict[str, Any]) -> str:
        type_code = _type_code(request)
        offset, width = int(request["offset"]), int(request.get("width", 2))
        stats = field_stats(self.repo, type_code, offset, width, bool(request.get("signed", True)))
        if stats is None:
            return "No shipped record of that type is long enough for that field."
        return ("Source: shipped Nova Data payloads (observed scenario records, not runtime frequency); "
                f"type {type_code.hex()}, +0x{offset:x}, {width} bytes big-endian.\n" + json.dumps(stats))

    def _resource(self, request: dict[str, Any]) -> str:
        """Raw big-endian resource bytes from the shipped archives."""
        type_code = _type_code(request)
        resource_id = int(request["id"]) if "id" in request else None
        offset, size = int(request.get("offset", 0)), int(request.get("size", 64))
        if offset < 0 or not 1 <= size <= 256:
            raise ValueError("resource window needs offset >= 0 and size in 1..256")
        lines = ["Source: shipped Nova Data archives (observed scenario bytes, not runtime frequency).",
                 f"Type {type_code.hex()}, payload window +0x{offset:x}, at most {size} bytes; resource fields are big-endian."]
        found = 0
        for path, archive in zip(archive_paths(self.repo), _archives(self.repo)):
            for tc, entry, rid, name in archive.map_records:
                if tc != type_code or (resource_id is not None and rid != resource_id):
                    continue
                base, length = archive.entries[entry - 1]
                payload = archive.data[base:base + length]
                lines.append(f"{path.name}: id {rid}, name {name!r}, payload size {length}, "
                             f"bytes[{offset}:{offset + size}]={payload[offset:offset + size].hex()}")
                found += 1
                if found >= MAX_RESOURCES:
                    return "\n".join(lines) + "\n[record limit reached; request a specific id]"
        return "\n".join(lines) + ("\nNo matching resources." if not found else "")


def make_plugin(project: Project) -> ScenarioPlugin:
    return ScenarioPlugin(project.root)
