#!/usr/bin/env python3
"""find_str_loads.py — Find decomp sites (analysis/*.c) that load .rez strings with hardcoded
resource ids, and resolve them against analysis/rez_str_mapping.json.

Helpers:
    Resource_LoadStringEntry(out, id, index)   0x004b8ca0
    LoadStringResourceCopyById(id)              0x004bc2a0
    Resource_AppendStringEntry(out, id, index) 0x004cd1a0
    Resource_DrawStringEntry(id, index)         0x004cd1f0

Only sites where the resource id is a hardcoded constant are reported and resolved. Sites passing
a runtime variable id are reported as "dynamic" (we cannot resolve them statically).

Usage:
    ./tools/find_str_loads.py                # human-readable report
    ./tools/find_str_loads.py --json         # write analysis/str_load_sites.json
    ./tools/find_str_loads.py --orig <id>    # grep where a specific resource id is loaded
"""

import os, re, json, argparse

HERE = os.path.dirname(os.path.abspath(__file__))
ANALYSIS = os.path.join(HERE, '..', 'analysis')
MAPPING = os.path.join(ANALYSIS, 'rez_str_mapping.json')

HELPERS = {
    'Resource_LoadStringEntry': (1,),    # hardcoded id is arg idx 1
    'LoadStringResourceCopyById': (0,),
    'Resource_AppendStringEntry': (1,),
    'Resource_DrawStringEntry': (0,),
}

# regex per helper: capture the full arg list up to the closing paren before ';'
PATS = {h: re.compile(r'\b' + h + r'\s*\((.*?)\)\s*;') for h in HELPERS}


def load_mapping():
    with open(MAPPING) as f:
        return json.load(f)


def all_c_files():
    for fn in sorted(os.listdir(ANALYSIS)):
        if fn.endswith('.c'):
            yield fn


def strip_comment(text):
    # naive strip of /* */ block comments (they contain the plate text, not calls)
    return re.sub(r'/\*.*?\*/', '', text, flags=re.S)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--orig', default=None, help='print all sites loading a specific id')
    ap.add_argument('--only', default=None, help='only scan one file (basename or addr prefix)')
    args = ap.parse_args()

    mapping = load_mapping()
    sites = []
    dynamic = []
    found_files = 0

    for fn in all_c_files():
        if args.only and args.only not in fn:
            continue
        path = os.path.join(ANALYSIS, fn)
        with open(path) as f:
            text = f.read()
        code = strip_comment(text)
        addr = fn.split('_')[0]
        if not any(h in code for h in HELPERS):
            continue
        found_files += 1
        for h, id_idx in HELPERS.items():
            pat = PATS[h]
            for em in pat.finditer(code):
                argvals = [a.strip() for a in em.group(1).split(',')]
                if id_idx[0] >= len(argvals):
                    continue
                idtok = argvals[id_idx[0]]
                mnum = re.fullmatch(r'(?:\(int\))?(?:\(short\))?(?:\(ushort\))?(0x[0-9a-fA-F]+|\d+)', idtok)
                if mnum:
                    val = int(mnum.group(1), 0)
                    info = mapping.get(str(val))
                    sites.append({
                        'file': fn, 'addr': addr, 'helper': h,
                        'call': ' '.join((''.join(a) for a in em.group(0).split())),
                        'id': val,
                        'pool': info['name'] if info else None,
                    })
                else:
                    dynamic.append({'file': fn, 'addr': addr, 'helper': h,
                                    'call': ' '.join(''.join(a) for a in em.group(0).split()),
                                    'idtoken': idtok})

    if args.orig is not None:
        tid = int(args.orig, 0)
        print(f"Sites loading resource id {tid} (0x{tid:04x}) — {mapping.get(str(tid),{}).get('name','?')}:\n")
        for s in sorted(sites, key=lambda t: t['addr']):
            if s['id'] == tid:
                print(f"  {s['addr']}  {s['file']}")
                print(f"      {s['call']}")
        return

    if args.json:
        out = os.path.join(ANALYSIS, 'str_load_sites.json')
        with open(out, 'w') as f:
            json.dump({'sites': sites, 'dynamic': dynamic}, f, indent=1)
        print(f"wrote {out} ({len(sites)} resolved sites, {len(dynamic)} dynamic)")
        return

    print(f"{'addr':<11} {'helper':<30} {'id':<6} ['pool']  (file)")
    print('-' * 100)
    for s in sorted(sites, key=lambda t: (t['id'], t['addr'])):
        print(f"{s['addr']}  {s['helper']:<30} {s['id']:<6} [{s['pool']}]  ({s['file']})")
    print()
    print(f"files with helper calls: {found_files}")
    print(f"resolved hardcoded-id sites: {len(sites)}")
    print(f"dynamic-id sites (not annotated): {len(dynamic)}")


if __name__ == '__main__':
    main()
