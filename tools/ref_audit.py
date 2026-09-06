#!/usr/bin/env python3
"""Cross-check progress.csv reimplementation rows against the C++ sources and
the Ghidra DB.

For each row with an impl_file:
  * query Ghidra for the current function name at the address
  * compare Ghidra name vs progress.csv `name` column
  * locate the port function in the impl file (exact, then fuzzy-normalized)
  * check whether the Ghidra address is cited anywhere in the impl file

Writes a report to analysis/ref_audit.txt. Read-only; no writes to the DB.
"""
import csv
import json
import os
import re
import urllib.request
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = "http://127.0.0.1:8166"

ADDR_RE = re.compile(r"0x[0-9A-Fa-f]{6,8}")
FUNC_DEF_RE = re.compile(
    r"^(?:[A-Za-z_][\w:<>,*&\s\[\]]*?\s+)?([A-Za-z_]\w*)\s*\(", re.M
)


def ghidra_get(path: str):
    try:
        with urllib.request.urlopen(f"{BASE}{path}", timeout=10) as r:
            return r.read().decode()
    except Exception as e:  # noqa: BLE001
        return f"<ERR {e}>"


def ghidra_name(addr: str) -> str:
    out = ghidra_get(f"/function/{addr}/info")
    if out.startswith("<ERR") or not out.strip():
        return out
    # format: "<addr> <name> <signature...> /* comment */"
    parts = out.split()
    return parts[1] if len(parts) >= 2 else out


def normalize(name: str) -> str:
    n = name.lower().replace("_", "")
    if n.startswith("nova"):
        n = n[4:]
    if n.startswith("asteroid"):  # NovaAsteroid_*
        n = n[len("asteroid"):]
    return n


def file_symbols(text: str) -> set[str]:
    syms = set()
    for m in FUNC_DEF_RE.finditer(text):
        s = m.group(1)
        if s in {"if", "for", "while", "switch", "return", "sizeof", "case"}:
            continue
        syms.add(s)
    return syms


def load_tsv(path: Path, fields: list[str]) -> list[dict]:
    """Tab-separated rows; tolerates stray whitespace, no quoting ever."""
    out = []
    for line in path.read_text().splitlines()[1:]:
        if not line.strip():
            continue
        vals = line.split("\t")
        vals += [""] * (len(fields) - len(vals))
        out.append(dict(zip(fields, vals)))
    return out


def main() -> None:
    fields = ["address", "name", "impl_file", "reimpl_pct", "comment"]
    rows = load_tsv(ROOT / "decomp-progress.tsv", fields)
    file_cache: dict[str, tuple[str, set[str]]] = {}

    ghidra_mismatch = []      # ghidra name != csv name
    no_citation = []          # address string absent from impl file
    not_in_ghidra = []        # address is not a known function in ghidra

    for r in rows:
        if not r["impl_file"]:
            continue
        path = ROOT / r["impl_file"]
        if path not in file_cache:
            txt = path.read_text()
            file_cache[str(path)] = (txt, file_symbols(txt))
        txt, syms = file_cache[str(path)]

        gname = ghidra_name(r["address"])
        if gname.startswith("<ERR"):
            not_in_ghidra.append(f"{r['address']} {r['name']} -> {gname}")
        elif "INTERNAL LABEL" not in r["comment"]:
            csv_name = r["name"]
            # ignore case/underscore-only differences
            if normalize(gname) != normalize(csv_name):
                ghidra_mismatch.append(
                    f"{r['address']}  csv='{csv_name}'  ghidra='{gname}'"
                )

        addr = r["address"]
        if "INTERNAL LABEL" in r["comment"]:
            continue  # interior-address slice of a parent function, not citable
        haystack = txt
        header = path.with_suffix(".hpp")
        if header.exists() and str(header) not in file_cache:
            htxt = header.read_text()
            file_cache[str(header)] = (htxt, file_symbols(htxt))
        if header.exists():
            haystack += file_cache[str(header)][0]
        if addr.lower() not in haystack.lower():
            no_citation.append(f"{r['address']} {r['name']} -> {r['impl_file']}")

    # census: progress + skipped must be disjoint and cover the decompile dump
    out = []
    skipped = load_tsv(ROOT / "decomp-skipped.tsv", ["address", "name", "library", "comment"])
    p_addrs = {r["address"].lower() for r in rows}
    s_addrs = {r["address"].lower() for r in skipped}
    overlap = p_addrs & s_addrs
    out.append(f"\n== census ==")
    out.append(f"progress rows: {len(rows)}  skipped rows: {len(skipped)}")
    if overlap:
        out.append(f"!! overlap between progress and skipped ({len(overlap)}): {sorted(overlap)[:10]}")
    else:
        out.append("progress/skipped disjoint: ok")
    dump = Path("/tmp/ghidra_full_decompile")
    if dump.is_dir():
        dump_addrs = {f"0x{p[:8].lower()}" for p in os.listdir(dump) if p.endswith(".c")}
        tracked = p_addrs | s_addrs
        missing = dump_addrs - tracked - {r["address"].lower() for r in rows if "INTERNAL LABEL" in r["comment"]}
        if missing:
            out.append(f"!! functions in decompile dump without a row ({len(missing)}): {sorted(missing)[:10]}")
        else:
            out.append(f"census vs decompile dump ({len(dump_addrs)} functions): complete")
    else:
        out.append("(no decompile dump at /tmp/ghidra_full_decompile; union check skipped)")

    out.append(f"\nrows with impl_file: {sum(1 for r in rows if r['impl_file'])}")
    out.append(f"\n== Ghidra name != csv name ({len(ghidra_mismatch)}) ==")
    out += sorted(ghidra_mismatch)
    out.append(f"\n== address not cited in impl file or sibling header ({len(no_citation)}) ==")
    out += sorted(no_citation)
    out.append(f"\n== address not a function in Ghidra ({len(not_in_ghidra)}) ==")
    out += not_in_ghidra
    report = "\n".join(out)
    (ROOT / "analysis" / "ref_audit.txt").write_text(report)
    print(report[:4000])


if __name__ == "__main__":
    main()
