#!/usr/bin/env python3
"""rez_strings_map.py — build master STR# resource-id -> indexed-string mapping for the EVN .rez
archives, for annotating the decomp.

Background
----------
The EV Nova engine loads STR# string pools from the .rez archives at *runtime* into
NovaHeap memory (see FUN_00501740 -> NovaHeap allocation in the Ghidra decomp), so
strings do NOT have stable static image addresses. The stable key the game uses for every
string is the (resource_id, index) pair passed to the string-loading helpers:

    Resource_LoadStringEntry(char *out, resource_id, index)   @ 0x004b8ca0
    LoadStringResourceCopyById(resource_id)                    @ 0x004bc2a0
    Resource_AppendStringEntry(out, resource_id, index)        @ 0x004cd1a0
    Resource_DrawStringEntry(resource_id, index)               @ 0x004cd1f0

So we treat resource_id as the address-like namespace. This file produces a
machine-readable id -> {index: string, ...} map (and a readable report).

Outputs
-------
  * --report [REZ...]  : human-readable id/name + indexed entries for each STR# pool
  * --json     [REZ...]: compact JSON id -> [[index, "string"], ...]
  * --lookup ID [REZ...]: print just the entries for one resource id (decimal or 0x hex)
  * --codeusages        : print every ghidra call to the four string-loading helpers with a
                          literal id, mapping each back to the pool. (needs --ghidra)
  * default (no mode) : write analysis/rez_str_mapping.json and a .txt report.

The JSON map is keyed by STR# resource id (as found in Nova Data 5.rez). Entries that
carry the crln control char (0xd9) in the on-disk names are re-marked here as <cron>.
"""

import sys, os, glob, json, argparse
from rez_extract import Archive, _u32, type_code_str  # reuse parser

HERE = os.path.dirname(os.path.abspath(__file__))


def default_paths():
    return sorted(glob.glob(os.path.join(HERE, '..', 'EV Nova', 'Nova Files', '*.rez')))


def load_all(paths=None):
    paths = paths or default_paths()
    arch = []
    for p in paths:
        try:
            arch.append(Archive(p))
        except (ValueError, OSError):
            continue
    return arch


def build_str_map(paths=None):
    """id -> {name, entries:[(index,str)]} for every STR# pool found across the archives."""
    mapping = {}
    for a in load_all(paths):
        pools = a.str_regions()
        records = {rid: name for _, rid, name in a.str_records()}
        idx = 0
        # str_records gives ordered (idx,rid,name); match them to regions in order
        recs = a.str_records()
        for k, off, size, pool in a.str_regions():
            rid = recs[idx][1] if idx < len(recs) else -1
            name = recs[idx][2] if idx < len(recs) else ''
            idx += 1
            mapping.setdefault(rid, {"name": name, "entries": []})
            for i, s in enumerate(pool):
                mapping[rid]["entries"].append((i, s))
    return mapping


def report(mapping):
    for rid in sorted(mapping):
        info = mapping[rid]
        print(f"id={rid} (0x{rid:04x})  {info['name']}  ({len(info['entries'])} strings)")
        for i, s in info['entries']:
            print(f"    [{i}]  {s!r}")


def find_ghidra(keys_only=False):
    """Helper to shell out to ghidra_api and find string-load helper call sites."""
    import subprocess
    help = {
        '004b8ca0': 'Resource_LoadStringEntry(out, id, index)',
        '004bc2a0': 'LoadStringResourceCopyById(id)',
        '004cd1a0': 'Resource_AppendStringEntry(out, id, index)',
        '004cd1f0': 'Resource_DrawStringEntry(id, index)',
    }
    out = {}
    for addr, desc in help.items():
        try:
            r = subprocess.run(['./tools/ghidra_api', f'function/{addr}/callers'],
                               capture_output=True, text=True, cwd=os.path.join(HERE, '..'))
            out[addr] = (desc, r.stdout)
        except Exception as e:
            out[addr] = (desc, f'error {e}')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', nargs='?', default=None,
                    help='report|json|lookup|codeusages (default: write analysis files)')
    ap.add_argument('args', nargs='*', help='resource id for lookup, or .rez path filters')
    args = ap.parse_args()

    if args.mode == 'codeusages':
        for addr, (desc, out) in find_ghidra().items():
            print(f"=== {addr} {desc} ===")
            print(out)
        return

    paths = None
    id_ = None
    for a in args.args:
        if a.lower().endswith('.rez') or os.sep in a:
            paths = [a]
        else:
            id_ = int(a, 0) if a.startswith('0x') else int(a)

    mapping = build_str_map(paths)

    if args.mode == 'json':
        print(json.dumps({str(k): v for k, v in mapping.items()}, indent=1, ensure_ascii=False))
        return
    if args.mode == 'report':
        report(mapping)
        return
    if args.mode == 'lookup':
        if id_ is None:
            print('lookup requires a resource id argument', file=sys.stderr); sys.exit(1)
        if id_ not in mapping:
            print(f'id {id_} (0x{id_:04x}): not found')
            return
        report({id_: mapping[id_]})
        return

    # default: write analysis artifacts
    os.makedirs(os.path.join(HERE, '..', 'analysis'), exist_ok=True)
    jp = os.path.join(HERE, '..', 'analysis', 'rez_str_mapping.json')
    with open(jp, 'w') as f:
        json.dump({str(k): v for k, v in mapping.items()}, f, indent=1, ensure_ascii=False)
    rp = os.path.join(HERE, '..', 'analysis', 'rez_str_mapping.txt')
    with open(rp, 'w') as f:
        buf = []
        for rid in sorted(mapping):
            info = mapping[rid]
            buf.append(f"# id={rid} (0x{rid:04x})  {info['name']}  ({len(info['entries'])} strings)")
            for i, s in info['entries']:
                buf.append(f"    [{i}]  {s!r}")
        f.write('\n'.join(buf) + '\n')
    print(f"wrote {jp}")
    print(f"wrote {rp}")
    print(f"total STR# pools: {len(mapping)}")


if __name__ == '__main__':
    main()
