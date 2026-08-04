#!/usr/bin/env python3
"""rez_extract.py — list and extract resources from EV Nova .rez archives.

The EVN .rez files are self-contained "BRGR" container archives. This tool parses them
directly (no engine dependency) using the on-disk layout reverse-engineered from the
game's Ghidra decomp:

  * Container header (little-endian):
        [0x00] b'BRGR' magic
        [0x04] version (u32, =1)
        [0x08] blob size (u32) — size of the region-descriptor table at 0x0c
  * Region-descriptor blob at 0x0c (little-endian): a 12-byte header [0x01,0x01,N],
    then 24-byte descriptor units. Each unit encodes up to two data regions:
        [off, size] [0, off+size] [next_size, 0]
    so region k = (a0, b0) and region k+1 = (a0+b0, a2). The final unit's (b1, a2)
    instead gives the resource.map's file offset and size.
  * resource.map (big-endian, may carry a small checksum preamble):
        [type_dir_offset(u32)][type_count(u32)]
        type entries (12 bytes each): [type_code(u32)][records_offset(u32)][record_count(u32)]
        type records (0x10a stride):   [index(u32)][type_code(u32)][res_id(u16)][name...]
  * STR# ('STR#' = 0x53545223) string resources are the interesting ones: their data is
    a big-endian [string_count(u16)] followed by Pascal strings (1-byte length prefixes).

Subcommands:
    list        List every resource grouped by type + resource id + name, per archive.
    strings     Decode and print all STR# string pools found in the given archive(s).
    types       Print just the resource types present per archive (quick overview).
    find WORD   Like strings but only print pools/entries whose text matches WORD.
"""

import sys, os, glob, argparse, struct

def be(f, o, n): return int.from_bytes(f[o:o+n], 'big')
def le(f, o, n): return int.from_bytes(f[o:o+n], 'little')

def type_code_str(tc):
    """Pretty-print a 4-byte resource type code."""
    s = "".join(chr(c) if 32 <= c < 127 else '.' for c in tc)
    return s

class Archive:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        with open(path, 'rb') as fh:
            self.data = fh.read()
        self._parse()

    def _parse(self):
        d = self.data
        if d[:4] != b'BRGR':
            raise ValueError("not a BRGR archive")
        self.version = le(d, 4, 4)
        self.blob_size = le(d, 8, 4)
        blob = d[0x0c:0x0c+self.blob_size]
        self.regions = []
        units = []
        p = 0x18 - 0x0c          # skip the 12-byte blob header
        last = None
        while p + 24 <= len(blob):
            a0, b0 = _u32(blob, p), _u32(blob, p+4)
            a1, b1 = _u32(blob, p+8), _u32(blob, p+12)
            a2, b2 = _u32(blob, p+16), _u32(blob, p+20)
            if a0 and a1 == 0 and b1 == a0 + b0:
                last = (a0, b0, a1, b1, a2, b2)
                units.append(last)
            else:
                break
            p += 24
        if last is None:
            raise ValueError("no region descriptors")
        self.map_off = last[3]
        self.map_size = last[4]
        # all regions: (a0,b0) from every unit; plus (b1,a2) from all but the final unit
        self.regions = [u[0:2] for u in units]
        for u in units[:-1]:
            if u[4]:
                self.regions.append((u[3], u[4]))
        self._parse_map()

    def _parse_map(self):
        d = self.data
        off = self.map_off
        n = None
        for lead in (0, 4):          # some archives carry a checksum preamble
            probe = off + lead
            if probe + 8 > len(d):
                continue
            t = be(d, probe+4, 4); o = be(d, probe, 4)
            if 1 <= t <= 500 and 0 <= o <= 0x10000:
                n = t; self.map_body = probe
                break
        if n is None:
            self.map_types = []; self.map_records = []
            return
        to = be(d, self.map_body, 4)
        ntypes = n
        types = []
        for gi in range(ntypes):
            e = self.map_body + to + gi*12
            if e + 12 > len(d):
                break
            tc = d[e:e+4]
            ro, rc = struct.unpack('>II', d[e+4:e+12])
            types.append((tc, ro, rc))
        self.map_types = types
        # read flattened records
        records = []
        for tc, ro, rc in types:
            start = self.map_body + ro
            for i in range(rc):
                r = start + i * 0x10a
                if r + 0x0c > len(d):
                    break
                idx = be(d, r, 4)
                rid = be(d, r+8, 2)
                name = d[r+0x0a:r+0x40].split(b'\x00')[0].decode('latin1', 'replace')
                records.append((tc, idx, rid, name))
        self.map_records = records

    # ---- helpers ---------------
    def decode_str_pool(self, off, size):
        """If the region at off is an STR# string pool, return the list of strings.
        Requires the pool to be largely printable text (avoids binary false positives)."""
        f = self.data
        if off + 2 >= len(f) or off + size > len(f):
            return None
        n = be(f, off, 2)
        if not (1 <= n <= 4000):
            return None
        out = []; p = off + 2; end = off + size
        printable = 0; total = 0
        for _ in range(n):
            if p >= end:
                return None
            L = f[p]
            if L == 0 or p + 1 + L > end:
                return None
            s = f[p+1:p+1+L].decode('latin1')
            total += L
            printable += sum(1 for c in s if c == ' ' or 32 <= ord(c) < 127)
            out.append(s); p += 1 + L
        # require majority printable and not all single-character noise
        if total == 0 or printable / total < 0.6:
            return None
        return out if p <= end else None

    def str_regions(self):
        """Return (region_index, offset, size, pool_strings) for every STR# pool region."""
        res = []
        for k, (off, size) in enumerate(self.regions):
            pool = self.decode_str_pool(off, size)
            if pool is not None:
                res.append((k, off, size, pool))
        return res

    def str_records(self):
        """The STR# (0x53545223) resource records: (idx, id, name)."""
        out = []
        for tc, idx, rid, name in self.map_records:
            if tc == b'STR#':
                out.append((idx, rid, name))
        return out


def _u32(b, o):
    return int.from_bytes(b[o:o+4], 'little')


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['list', 'strings', 'strings-all', 'types', 'find'],
                    help="operation to run")
    ap.add_argument('pattern', nargs='?', default=None,
                    help="search word (find mode); optional archive name filter")
    ap.add_argument('paths', nargs='*', default=None,
                    help="specific .rez files; defaults to all 'EV Nova/Nova Files/*.rez'")
    return ap.parse_args()


def default_paths():
    here = os.path.dirname(os.path.abspath(__file__))
    return sorted(glob.glob(os.path.join(here, '..', 'EV Nova', 'Nova Files', '*.rez')))


def main():
    args = parse_args()
    paths = args.paths or default_paths()
    if args.mode == 'find':
        # find takes a word then optional file paths; 'pattern' is the word
        paths = args.paths or default_paths()
        word = args.pattern
    else:
        # other modes: any positional after the mode is a file path filter
        word = None
        if args.pattern and (args.pattern.lower().endswith('.rez') or os.sep in args.pattern):
            paths = [args.pattern]
    archives = []
    for p in paths:
        try:
            archives.append(Archive(p))
        except (ValueError, OSError) as e:
            print(f"# {os.path.basename(p)}: {e}", file=sys.stderr)
    if args.mode == 'types':
        for a in archives:
            if not a.map_types:
                print(f"{a.name}: <no parseable resource.map>")
                continue
            t = ', '.join(f"{type_code_str(tc)}({rc})" for tc, ro, rc in a.map_types)
            print(f"{a.name:22s} total_records={len(a.map_records)}  {t}")
        return
    if args.mode == 'list':
        for a in archives:
            print(f"\n===== {a.name}  (map@{hex(a.map_off)}, {len(a.map_records)} resources) =====")
            if not a.map_types:
                print("  <no parseable resource.map>")
                continue
            pos = 0
            for tc, ro, rc in a.map_types:
                print(f"  type '{type_code_str(tc)}' ({tc.hex()})  {rc} resources")
                for _ in range(rc):
                    if pos >= len(a.map_records):
                        break
                    _, idx, rid, name = a.map_records[pos]
                    print(f"      id={rid:5d} (0x{rid:04x})  idx={idx:<4d}  {name}")
                    pos += 1
        return
    if args.mode == 'strings' or args.mode == 'strings-all':
        for a in archives:
            pools = a.str_regions()
            if not pools:
                print(f"{a.name}: no STR# string pools")
                continue
            str_recs = a.str_records()
            print(f"\n===== {a.name}  ({len(pools)} STR# string pools) =====")
            if str_recs:
                print("  STR# resources (id -> name):")
                for idx, rid, name in str_recs:
                    print(f"      id={rid:5d} (0x{rid:04x})  idx={idx:<4d}  {name}")
            for k, off, size, pool in pools:
                print(f"  region#{k}  off={hex(off)} size={hex(size)}  {len(pool)} strings")
                if args.mode == 'strings-all' or len(pool) <= 25:
                    for s in pool:
                        print(f"      {s!r}")
                else:
                    print(f"      ({len(pool)} strings; use strings-all to print all)")
        return
    if args.mode == 'find':
        if not word:
            print('find requires a word', file=sys.stderr)
            sys.exit(1)
        for a in archives:
            str_recs = a.str_records()
            for k, off, size, pool in a.str_regions():
                hits = [s for s in pool if word.lower() in s.lower()]
                if hits:
                    print(f"{a.name} region#{k} off={hex(off)}:")
                    for s in hits:
                        print(f"    {s!r}")


if __name__ == '__main__':
    main()
