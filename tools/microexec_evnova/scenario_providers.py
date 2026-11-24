"""Inject verified stock scenario records as optional microexec generators.

The evidence plugin stays read-only. Records coexist with synthetic samplers;
Jev and fuzz choose their source. No dynamic state or relationships are imposed.

Two provider families are generated:

* governments, from a hand-verified payload window (`government_record`);
* every other numeric family, from the port decoder's *clean direct copies*:
  a field whose only appearance in `src/game/scenario_data.cpp` is a single
  `ReadBeNN(bytes, <literal>)` assignment, whose Ghidra struct field is a scalar
  at least as wide as that read, and whose name resolves through the bridge
  rules. Derived,
  rebased, clamped, float and string fields are deliberately omitted so a
  partial record is always a factual payload copy; omitted fields fall through
  to the normal generators.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "microexec" / "src"))

from ghidra import FLOATS, PRIMITIVE_SIZES, is_pointer, split_array
from model import Candidate
from providers import Provider, register, validate_record
from scenario import decoder_source, records

# Original runtime table prefix. Provider ids use `scenario:<family>:<resource id>`;
# the whole prefix is this adapter's set, so removal and reinstallation stay scoped.
PREFIX = "scenario:"


@dataclass(frozen=True)
class Family:
    key: str            # provider id segment, e.g. "ships"
    global_name: str    # original fixed table global
    type_code: bytes    # resource type
    type_name: str      # Ghidra struct/type of one table element
    decoder: str        # port decoder for the payload -> runtime field map
    count: int          # original fixed table length (resource ids 0x80-based)


FAMILIES: tuple[Family, ...] = (
    Family("ships", "g_ship_class_defs", b"sh\x95p", "ShipClassDef", "DecodeShip", 0x300),
    Family("outfits", "g_outfit_defs", b"o\x9ftf", "OutfitDef", "DecodeOutfit", 0x200),
    Family("weapons", "g_weapon_defs", b"w\x91ap", "WeaponDef", "DecodeWeapon", 0x100),
    Family("stellars", "g_stellar_defs", b"sp\x9ab", "StellarDef", "DecodeStellar", 0x800),
    Family("systems", "g_system_defs_ptr", bytes.fromhex("73d87374"), "SystemDef", "DecodeSystem", 0x800),
    Family("fleets", "g_random_encounter_fleet_defs", b"fl\x91t", "RandomEncounterFleetDef", "DecodeFleet", 0x100),
    Family("dudes", "g_dude_defs", bytes.fromhex("649f6465"), "DudeDef", "DecodeDude", 0x200),
    Family("pers", "g_pers_defs", bytes.fromhex("70917273"), "PersDef", "DecodePers", 0x400),
)

# One `member.field = ReadBe(I){8,16,32}(bytes, <literal offset>);` assignment.
_READ = re.compile(
    r"\b(\w+)\.(?P<field>\w+)\s*=\s*ReadBe(?P<signed>I?)(?P<bits>8|16|32)"
    r"\(\s*bytes\s*,\s*(?P<offset>0x[0-9a-fA-F]+|\d+)\s*\)\s*;")


def government_record(payload: bytes) -> dict[str, int]:
    """Verified subset of GovtDef, in the original runtime domain.

    Ghidra 0x004bd3c0 copies flags +02 -> +20 and class/ally/enemy
    +18/+20/+28 -> +26/+2e/+36 without rebasing class numbers.
    Dynamic fields and all unrelated decoder fields are deliberately omitted.
    """
    if len(payload) < 0x30:
        raise ValueError("government payload is shorter than its relation fields")
    result = {"flags_primary": int.from_bytes(payload[2:4], "big")}
    for stem, offset in (("class", 0x18), ("ally_class", 0x20), ("enemy_class", 0x28)):
        for i in range(4):
            start = offset + i * 2
            result[f"{stem}_{i + 1}"] = int.from_bytes(payload[start:start + 2], "big", signed=True)
    return result


def government_providers(repo: Path) -> dict[str, Provider]:
    """One exact partial record per shipped id; retain holes and index identity."""
    definitions = {}
    for rid, payload in records(repo, bytes.fromhex("679a7674")):
        if not 128 <= rid < 384:
            raise ValueError(f"government resource id {rid} is outside the runtime table")
        provider = Provider(
            f"{PREFIX}government:{rid}", "parent_sampler", f"g_government_defs[{rid - 128}]",
            [Candidate(f"resource:{rid}", "record", government_record(payload), prior_evidence="observed")],
            alternative=True,
            text=f"Stock Nova Data govt resource {rid}; verified flags and relation copies in "
                 "NovaData_LoadScenarioResourceTables (0x004bd3c0). Class numbers are not resource ids.")
        # Later archives shadow earlier resources, as the original loader does.
        definitions[provider.id] = provider
    if not definitions:
        raise ValueError("no stock government resources found")
    return definitions


def validate_governments(ghidra, definitions: dict[str, Provider]) -> None:
    layout = ghidra.layout("GovtDef")
    expected = {"flags_primary": (0x20, "ushort")}
    expected.update({f"{stem}_{i + 1}": (offset + i * 2, "short")
                     for stem, offset in (("class", 0x26), ("ally_class", 0x2e), ("enemy_class", 0x36))
                     for i in range(4)})
    actual = {field.name: (field.offset, field.type) for field in layout.fields} if layout else {}
    if any(actual.get(name) != position for name, position in expected.items()):
        raise ValueError("GovtDef no longer matches the verified flags/relation layout")
    for provider in definitions.values():
        validate_record(ghidra, "GovtDef", provider.candidates[0].value)


def _strip_comments(text: str) -> str:
    return "\n".join(re.sub(r"//.*", "", line) for line in text.splitlines())


def _bridge_aliases(type_name: str) -> dict[str, str]:
    """Port decoder field -> Ghidra field for plain renames in rules.json.

    Only string rules and `path`-only rules without a rebasing `add`/`preserve`
    are usable: a provider record is a memory value, not a translation.
    """
    rules = json.loads((HERE / "rules.json").read_text())
    aliases: dict[str, str] = {}
    for ghidra_field, mapping in rules.get("types", {}).get(type_name, {}).get("fields", {}).items():
        if isinstance(mapping, str):
            field = mapping
        elif isinstance(mapping, dict) and "path" in mapping and "add" not in mapping and "preserve" not in mapping:
            field = mapping["path"]
        else:
            continue
        if "[" in field or "." in field or "[" in ghidra_field:
            continue
        aliases.setdefault(field, ghidra_field)
    return aliases


def direct_fields(source: str, layout, aliases: dict[str, str]) -> dict[str, tuple[int, int, bool]]:
    """Ghidra field -> (payload offset, payload width, signed) for clean copies.

    A field qualifies only when its sole appearance in the decoder is one
    literal-offset `ReadBeNN` assignment (no later use, clamp or rebase), the
    matching Ghidra field is a non-float scalar at least as wide as the payload
    read, and its name is unambiguous between the identity layout and the
    bridge aliases. A narrower payload into a wider integer is the loader's
    sign-extension (e.g. `short base_shield` -> `int base_shield_points`).
    """
    code = _strip_comments(source)
    fields = {field.name: field for field in layout.fields}
    out: dict[str, tuple[int, int, bool]] = {}
    for match in _READ.finditer(code):
        field = match.group("field")
        if len(re.findall(rf"\.{re.escape(field)}\b", code)) != 1:
            continue
        names = [name for name in (field, aliases.get(field)) if name in fields]
        if len(set(names)) != 1:
            continue
        ghidra_field = names[0]
        layout_field = fields[ghidra_field]
        if (layout_field.type in FLOATS or layout_field.type not in PRIMITIVE_SIZES
                or is_pointer(layout_field.type) or split_array(layout_field.type) is not None):
            continue
        width = int(match.group("bits")) // 8
        if width > layout_field.size:
            continue
        offset = int(match.group("offset"), 0)
        if ghidra_field in out:
            raise ValueError(f"{ghidra_field} maps to more than one payload field")
        out[ghidra_field] = (offset, width, bool(match.group("signed")))
    return out


def _read_field(payload: bytes, offset: int, width: int, signed: bool) -> int | None:
    chunk = payload[offset:offset + width]
    if len(chunk) < width:
        return None
    return int.from_bytes(chunk, "big", signed=signed)


def family_providers(repo: Path, ghidra, family: Family) -> dict[str, Provider]:
    """One observed partial record per shipped resource, in the original domain."""
    layout = ghidra.layout(family.type_name)
    if layout is None:
        raise ValueError(f"{family.type_name} is not a known Ghidra type")
    fields = direct_fields(decoder_source(repo, family.decoder) or "", layout,
                           _bridge_aliases(family.type_name))
    if not fields:
        raise ValueError(f"{family.decoder} exposes no clean direct payload copies")
    definitions: dict[str, Provider] = {}
    for rid, payload in records(repo, family.type_code):
        if not 128 <= rid < 128 + family.count:
            raise ValueError(f"{family.key} resource id {rid} is outside the runtime table")
        record = {name: value for name, (offset, width, signed) in fields.items()
                  if (value := _read_field(payload, offset, width, signed)) is not None}
        if not record:
            continue
        validate_record(ghidra, family.type_name, record)
        provider = Provider(
            f"{PREFIX}{family.key}:{rid}", "parent_sampler", f"{family.global_name}[{rid - 128}]",
            [Candidate(f"resource:{rid}", "record", record, prior_evidence="observed")],
            alternative=True,
            text=f"Stock Nova Data {family.key} resource {rid}; {len(record)} verified payload copies "
                 f"into {family.type_name} via {family.decoder} and NovaData_LoadScenarioResourceTables "
                 "(0x004bd3c0). Derived, rebased, clamped and string fields are omitted.")
        definitions[provider.id] = provider
    if not definitions:
        raise ValueError(f"no stock {family.key} resources found")
    return definitions


def scenario_providers(repo: Path, ghidra, families: tuple[Family, ...] = FAMILIES) -> dict[str, Provider]:
    """Every stock provider this adapter installs, keyed by provider id."""
    definitions = government_providers(repo)
    validate_governments(ghidra, definitions)
    for family in families:
        definitions |= family_providers(repo, ghidra, family)
    return definitions


def select_providers(db, definitions: dict[str, Provider]) -> None:
    """Synchronize this adapter's set, leaving all other generators and history intact.

    An empty set removes every stock provider from the world and copied target
    ledgers. Reinstalling unchanged definitions preserves their accumulated
    evidence.
    """
    from world import World

    def sync(existing):
        selected = {key: value for key, value in existing.items()
                    if not key.startswith(PREFIX) or key in definitions}
        for provider in definitions.values():
            # register preserves evidence, so give every ledger its own copy.
            register(selected, Provider.from_dict(asdict(provider)))
        return selected

    with db.locked(), db.tx():
        world = World(db)
        world.providers = sync(world.providers)
        world.save()
        for store in db.stores():
            ledger = store.load_ledger()
            before = {key: asdict(value) for key, value in ledger.providers.items()}
            ledger.providers = sync(ledger.providers)
            if before != {key: asdict(value) for key, value in ledger.providers.items()}:
                store.save_ledger(ledger)
        db.commit("inject stock scenario generator options" if definitions else "remove stock scenario providers",
                  cause="scenario-providers")


def main() -> None:
    from ghidra import Ghidra
    from project import load
    from store import StateDB

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, help="override the profile's microexec state directory")
    parser.add_argument("--check", action="store_true", help="validate against Ghidra without installing")
    parser.add_argument("--remove", action="store_true", help="remove stock scenario providers; keep other generators")
    parser.add_argument("--family", action="append", choices=[f.key for f in FAMILIES],
                        help="restrict generation/validation to a family; repeatable (governments always included)")
    args = parser.parse_args()
    project = load(HERE / "microexec.toml")
    state = (args.state_dir or project.state_dir).resolve()
    if args.check and args.remove:
        parser.error("--check and --remove cannot be combined")
    families = tuple(f for f in FAMILIES if not args.family or f.key in args.family)
    definitions: dict[str, Provider] = {}
    if not args.remove:
        ghidra = Ghidra(state / "cache", base=project.ghidra, refresh=True)
        definitions = scenario_providers(project.root, ghidra, families)
    if args.check:
        print(f"Validated {len(definitions)} stock scenario providers against their Ghidra types")
        return
    db = StateDB(state)
    try:
        select_providers(db, definitions)
    finally:
        db.close()
    print(f"Injected {len(definitions)} stock scenario generator options in {state}")


if __name__ == "__main__":
    main()
