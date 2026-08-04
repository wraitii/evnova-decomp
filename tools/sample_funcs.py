#!/usr/bin/env python3
"""Random-sample functions from a Ghidra address range and flag metadata gaps.

Check categories (per sampled function):
  [UNNAMED]     - function name looks decompiler-auto-generated (FUN_*, func_*)
  [NOCOMMENT]   - no /* plate comment */ present
  [PROVISIONAL] - comment explicitly marks Provisional or low confidence
  [UNKNOWN_RET] - return type is an unresolved/undefined type
  [GENERIC_PARAMS] - params still named param_N / aN (untyped)

Usage:
  python3 tools/sample_funcs.py [count] [seed]
Default: count=20, seed=random. Prints flagged first, then clean.
"""

import re
import sys
import urllib.parse
import urllib.request

BASE = "http://127.0.0.1:8166"
RANGE_START = 0x00400000
RANGE_END = 0x00480000
PAGE = 1000

UNNAMED_RE = re.compile(r"(?:FUN_|func_|undefined|DBG_)", re.I)
UNKNOWN_RET_RE = re.compile(r"^(?:undefined\d*|void\s*\*)", re.I)
GENERIC_PARAM_RE = re.compile(r"\b(?:param_\d+|a\d+|b\d+|c\d+)\b")
PROVISIONAL_RE = re.compile(r"provisional|low confidence|uncertain|unknown", re.I)


def http(method, path, body=None):
    req = urllib.request.Request(BASE + path, method=method)
    data = None
    if body is not None:
        data = json_dumps(body).encode()
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, data=data) as resp:
            return resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.read().decode()


def json_dumps(o):
    import json
    return json.dumps(o)


RECORD_START_RE = re.compile(r"^(?P<addr>[0-9a-fA-F]{8})\s+(?P<name>\S+)")


def parse_records(txt):
    out = []
    current = None
    for raw in txt.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = RECORD_START_RE.match(line)
        if m:
            if current is not None:
                out.append(current)
            current = (int(m.group("addr"), 16), m.group("name"), line)
        else:
            if current is not None:
                current = (current[0], current[1], current[2] + " " + line)
    if current is not None:
        out.append(current)
    return out


def get_functions():
    funcs = []
    start = RANGE_START
    while True:
        txt = http(
            "GET",
            f"/functions?start=0x{start:x}&end=0x{RANGE_END:x}&limit={PAGE}",
        )
        page = parse_records(txt if txt else "")
        if not page:
            break
        funcs.extend(page)
        last_addr = funcs[-1][0]
        if last_addr >= RANGE_END - 1:
            break
        if len(page) < PAGE:
            break
        start = last_addr + 1
    return funcs


def extract_comment(header):
    m = re.search(r"/\*(.*?)\*/", header, re.S)
    return m.group(1) if m else ""


def extract_signature(header):
    m = re.search(r"\)\s*(?:/\*.*)?$", header)
    m2 = re.search(r"\(.*\)", header, re.S)
    return m2.group(0) if m2 else ""


def classify(addr, name, header):
    issues = []
    # header format:  <addr> <name> <ret> <conv> <sig> /* comment */
    ret = ""
    sig = ""
    # split first 4 fields
    parts = header.split(None, 4)
    if len(parts) >= 4:
        ret = parts[2]
    if len(parts) >= 5:
        sig = parts[4]

    if UNNAMED_RE.search(name):
        issues.append("UNNAMED")
    # unbounded auto-name also flagged if it has no comment but skip (covered by NOCOMMENT)

    comment = extract_comment(header)
    if not comment.strip():
        issues.append("NOCOMMENT")
    elif PROVISIONAL_RE.search(comment):
        issues.append("PROVISIONAL")

    if UNKNOWN_RET_RE.match(ret.strip() if isinstance(ret, str) else ""):
        issues.append("UNKNOWN_RET")

    if GENERIC_PARAM_RE.search(sig):
        issues.append("GENERIC_PARAMS")

    return issues


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else None
    import random
    rng = random.Random(seed)

    funcs = get_functions()
    print(f"Total functions in range: {len(funcs)}", file=sys.stderr)

    sample = rng.sample(funcs, min(count, len(funcs)))

    results = []
    for addr, name, header in sample:
        issues = classify(addr, name, header)
        results.append((addr, name, header, issues))

    flagged = [r for r in results if r[3]]
    clean = [r for r in results if not r[3]]

    print(f"\n=== {len(flagged)} FLAGGED of {len(results)} sampled ===\n")
    for addr, name, header, issues in flagged:
        print(f"0x{addr:08x}  {name}")
        print(f"    issues: {', '.join(issues)}")
        print(f"    sig:    {extract_signature(header)}")
        print()
    print(f"\n=== {len(clean)} CLEAN (no heuristic flags) ===\n")
    for addr, name, header, issues in clean:
        print(f"0x{addr:08x}  {name}")
        print(f"    sig: {extract_signature(header)}")


if __name__ == "__main__":
    main()
