#!/usr/bin/env python3
"""rez_extract.py — list and extract resources from EV Nova .rez archives.

The EVN .rez files are self-contained "BRGR" container archives. This tool parses them
directly (no engine dependency) using the on-disk layout reverse-engineered from the
game's Ghidra decomp (ResourceArchive_OpenRez / BrgrResource_CloneAndRelocateTable).
The container model is mirror-marched against the C++ reconstruction in
`src/brgr_archive.cpp` so the two stay in lock-step.

Container layout (little-endian):
  [0x00] b'BRGR' magic
  [0x08] descriptor-blob size (u32)
  [0x0c] blob: [u32 1][u32 1][u32 entry_count]
         then entry_count * 12-byte entries: [u32 offset][u32 size][u32 name]
         (a third field is the entry's name; resource.map is always one of these
         region entries).
  Each entry (offset, size) is one data region inside the archive file.

resource.map (big-endian; the container region whose payload parses as a map
header, always the final entry):
  [u32 type_dir_offset][u32 type_count]
  type entries (12 bytes each) at map_base + type_dir_offset + n*12:
      [u32 type_code][u32 records_offset][u32 record_count]
      records_offset is relative to the map region's base offset in the file.
  records (0x10a bytes each) at map_base + records_offset + n*0x10a:
      [u32 index][u32 type_code][u16 res_id][name...  (NUL-terminated C string)]
  A record's `index` is 1-based into the container entry table, so the payload
  is the corresponding (offset, size) region.

A genuine resource.map region must satisfy for *every* type entry: record_count
in [1, 500], records_offset + record_count*0x10a inside the region. This
bounds check is what the naive "pick the first plausible header" heuristic
missed, which is why Nova Data 3/4, Graphics 1/3, Ships 2/3/5 and Titles 1
previously had *no* parseable map (their map is not the first candidate). The
C++ parser (brgr_archive.cpp) applies the same full-table validation, so the
two agree on all 21 shipped archives.

Subcommands:
    list          List every resource grouped by type + resource id + name, per archive.
    list-all      Like `list` but print the resource id list for every type
                  compactly across all archives (one line per type/archive).
    strings       Decode and print all STR# string pools found in the given archive(s).
    types         Print just the resource types present per archive (quick overview).
    find WORD     Like strings but only print pools/entries whose text matches WORD.
    summary       One line per archive with total records + per-type counts.
"""

import sys, os, glob, argparse, struct

def be(f, o, n): return int.from_bytes(f[o:o+n], 'big')
def le(f, o, n): return int.from_bytes(f[o:o+n], 'little')

BYTE = None

def type_code_str(tc):
    """Pretty-print a 4-byte resource type code."""
    return "".join(chr(c) if 32 <= c < 127 else '.' for c in tc)


def _map_candidate(f, off, size):
    """Return True if the region at file offset `off` is a coherent resource.map.

    Mirrors the C++ `ParseArchive` full-table, in-bounds validation: every type
    entry's record table must lie entirely inside the region and carry a
    plausible record count. This is the crucial check that distinguishes the real
    map from the many spurious little-endian regions that merely *look* like a
    [type_dir_offset][type_count] header.
    """
    if size < 8:
        return False
    tdo = be(f, off, 4)
    tc = be(f, off + 4, 4)
    if tc == 0 or tc > 500 or tdo > size or tdo + tc * 12 > size:
        return False
    for t in range(tc):
        type_entry = off + tdo + t * 12
        ro = be(f, type_entry + 4, 4)
        rc = be(f, type_entry + 8, 4)
        if ro > size or ro + rc * 0x10a > size:
            return False
    return True


class Archive:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        with open(path, 'rb') as fh:
            self.data = fh.read()
        self._parse()

    # ---- container (entry table) -------------------------------------------
    def _parse(self):
        d = self.data
        if d[:4] != b'BRGR':
            raise ValueError("not a BRGR archive")
        self.version = le(d, 4, 4)
        self.blob_size = le(d, 8, 4)
        blob = d[0x0c:0x0c + self.blob_size]
        if len(blob) < 16:
            raise ValueError("truncated descriptor blob")
        self.entry_count = le(blob, 8, 4)
        self.entries = []          # list of (offset, size)
        self.map_entry = -1        # index into self.entries of resource.map
        entries_begin = 0x0c + 12
        for i in range(self.entry_count):
            e = entries_begin + i * 12
            offset = le(d, e, 4)
            size = le(d, e + 4, 4)
            if offset > len(d) or size > len(d) - offset:
                raise ValueError("out-of-bounds region entry %d" % i)
            self.entries.append((offset, size))
        # Find the resource.map: scan the region entries for the one whose
        # payload is a coherent big-endian map header+table.
        for i, (offset, size) in enumerate(self.entries):
            if _map_candidate(d, offset, size):
                self.map_entry = i
                self.map_off = offset
                self.map_size = size
                break
        else:
            raise ValueError("no parseable resource.map")
        self._parse_map()

    # ---- resource.map -------------------------------------------------------
    def _parse_map(self):
        d = self.data
        off = self.map_off
        size = self.map_size
        body = off                     # no checksum preamble in BRGR maps
        tdo = be(d, body, 4)
        ntypes = be(d, body + 4, 4)
        types = []
        for g in range(ntypes):
            e = body + tdo + g * 12
            if e + 12 > len(d):
                break
            tc = d[e:e + 4]
            ro, rc = struct.unpack('>II', d[e + 4:e + 12])
            types.append((tc, ro, rc))
        self.map_types = types
        records = []
        for tc, ro, rc in types:
            start = body + ro          # records_offset is relative to map base
            for i in range(rc):
                r = start + i * 0x10a
                if r + 0x0c > len(d):
                    break
                idx = be(d, r, 4)
                rid = be(d, r + 8, 2)
                name = d[r + 0x0a:r + 0x40].split(b'\x00')[0].decode('latin1', 'replace')
                records.append((tc, idx, rid, name))
        self.map_records = records

    # ---- payload/region lookup -----------------------------------------------
    def resource_region(self, idx):
        """Return the file offset+size for a 1-based resource record index."""
        if idx < 1 or idx - 1 >= len(self.entries):
            return None
        return self.entries[idx - 1]

    def payload(self, idx):
        reg = self.resource_region(idx)
        if reg is None:
            return None
        off, size = reg
        return self.data[off:off + size]

    # ---- STR# string pools --------------------------------------------------
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
            s = f[p + 1:p + 1 + L].decode('latin1')
            total += L
            printable += sum(1 for c in s if c == ' ' or 32 <= ord(c) < 127)
            out.append(s); p += 1 + L
        if total == 0 or printable / total < 0.6:
            return None
        return out if p <= end else None

    def str_regions(self):
        """Return (region_index, offset, size, pool_strings) for every STR# pool
        region, i.e. every record of type STR# whose payload decodes as a pool."""
        res = []
        # For STR# records the payload region is the string pool body, so walk
        # the STR# records directly (content-addressed like the engine does).
        for tc, idx, rid, name in self.map_records:
            if tc != b'STR#':
                continue
            reg = self.resource_region(idx)
            if reg is None:
                continue
            off, size = reg
            pool = self.decode_str_pool(off, size)
            if pool is not None:
                res.append((idx, off, size, pool))
        return res

    def str_records(self):
        """The STR# (0x53545223) resource records: (idx, id, name)."""
        return [(idx, rid, name) for tc, idx, rid, name in self.map_records
                if tc == b'STR#']


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['list', 'list-all', 'strings', 'strings-all',
                                     'types', 'find', 'summary', 'dlg'],
                    help="operation to run")
    ap.add_argument('pattern', nargs='?', default=None,
                    help="search word (find mode); optional archive name filter")
    ap.add_argument('paths', nargs='*', default=None,
                    help="specific .rez files; defaults to all 'EV Nova/Nova Files/*.rez'")
    return ap.parse_args()


def default_paths():
    here = os.path.dirname(os.path.abspath(__file__))
    nova_root = os.path.join(here, '..', 'EV Nova')
    # Nova.rez (the core UI archive holding DLOG/DITL/MENU/...) sits directly in
    # EV Nova/, while the scenario/graphics archives sit under EV Nova/Nova Files/.
    paths = glob.glob(os.path.join(nova_root, 'Nova.rez'))
    paths += glob.glob(os.path.join(nova_root, 'Nova Files', '*.rez'))
    return sorted(set(paths))


def main():
    args = parse_args()
    paths = args.paths or default_paths()
    if args.mode == 'find':
        word = args.pattern
    else:
        word = None
        if args.pattern and (args.pattern.lower().endswith('.rez') or os.sep in args.pattern):
            paths = [args.pattern]
    archives = []
    for p in paths:
        try:
            archives.append(Archive(p))
        except (ValueError, OSError) as e:
            print(f"# {os.path.basename(p)}: {e}", file=sys.stderr)
    if not archives:
        return

    if args.mode == 'types':
        for a in archives:
            t = ', '.join(f"{type_code_str(tc)}({rc})" for tc, ro, rc in a.map_types)
            print(f"{a.name:22s} total_records={len(a.map_records)}  map@{hex(a.map_off)}  {t}")
        return

    if args.mode == 'summary':
        for a in archives:
            print(f"{a.name:22s} records={len(a.map_records):5d}  "
                  + "  ".join(f"{type_code_str(tc)}={rc}" for tc, ro, rc in a.map_types))
        return

    if args.mode == 'list':
        for a in archives:
            print(f"\n===== {a.name}  (map@{hex(a.map_off)}, {len(a.map_records)} resources) =====")
            for tc, ro, rc in a.map_types:
                print(f"  type '{type_code_str(tc)}' ({tc.hex()})  {rc} resources")
                shown = 0
                for _tc, idx, rid, name in a.map_records:
                    if _tc != tc:
                        continue
                    print(f"      id={rid:5d} (0x{rid:04x})  idx={idx:<4d}  {name}")
                    shown += 1
        return

    if args.mode == 'list-all':
        # Compact: one PICT/STR#/etc id list per archive, grouped by type.
        for a in archives:
            print(f"\n===== {a.name}  ({len(a.map_records)} resources) =====")
            for tc, ro, rc in a.map_types:
                ids = [f"{rid:#06x}" for _tc, idx, rid, name in a.map_records if _tc == tc]
                print(f"  {type_code_str(tc)} ({rc}): " + " ".join(ids))
        return

    if args.mode == 'strings' or args.mode == 'strings-all':
        for a in archives:
            pools = a.str_regions()
            str_recs = a.str_records()
            if not pools:
                print(f"{a.name}: no STR# string pools")
                continue
            print(f"\n===== {a.name}  ({len(pools)} STR# string pools) =====")
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

    if args.mode == 'dlg':
        # Decode the DLOG/DITL dialog resources from the core UI archive.
        # DLOG selects a DITL by id; a DITL (classic Mac DlgTemplate) is a
        # big-endian item list: [count:u16][version:u16] then per item
        # [top,i16][left,i16][bottom,i16][right,i16][type:u8][payload]. Payload is
        # a PascalString title for text/button/icon types, else a 5-byte refCon.
        import struct
        ITEM = {1: 'UserItem', 2: 'frame', 3: 'Icon?(Button)', 4: 'Button',
                5: 'CheckBox', 6: 'RadioButton', 7: 'ScrollBar', 8: 'StaticText',
                9: 'EditText', 11: 'Icon', 0xD: 'User', 0x20: 'Gauge', 0x40: 'Slider'}
        for a in archives:
            dlog = {rid: idx for tc, idx, rid, _ in a.map_records if tc == b'DLOG'}
            ditl = {rid: idx for tc, idx, rid, _ in a.map_records if tc == b'DITL'}
            if not (dlog or ditl):
                continue
            print(f"\n===== {a.name} dialogs =====")
            for rid, idx in sorted(dlog.items()):
                d = a.payload(idx)
                bnds = struct.unpack('>4h', d[2:10])
                dlog_id = int.from_bytes(d[0x12:0x14], 'big')
                print(f"  DLOG {rid:#06x} bounds=({bnds[1]},{bnds[0]}..{bnds[3]},{bnds[2]}) "
                      f"{bnds[3]-bnds[1]}x{bnds[2]-bnds[0]} -> DITL {dlog_id:#06x}")
            for rid, idx in sorted(ditl.items()):
                data = a.payload(idx)
                n = struct.unpack('>H', data[0:2])[0]
                print(f"  DITL {rid:#06x}  {n} items")
                q = 4
                for i in range(n):
                    if q + 10 > len(data):
                        break
                    top, left, bot, right = struct.unpack('>4h', data[q:q+8]); q += 8
                    it = data[q]; q += 1
                    title = ''
                    if it in (4, 5, 6, 8, 9, 11, 12, 0xD, 0x10, 0x14, 0x15, 0x21, 0x22, 0x24, 0x2d):
                        L = data[q]; title = data[q+1:q+1+L].decode('latin1'); q += 1 + L
                    else:
                        q += 5
                    print(f"      [{i:2d}] x={left:>4}..{right:>4} y={top:>4}..{bot:>4} "
                          f"{right-left:>3}x{bot-top:>3} type={it:#04x} {ITEM.get(it,'?'):<10} {title!r}")
        return


if __name__ == '__main__':
    main()
