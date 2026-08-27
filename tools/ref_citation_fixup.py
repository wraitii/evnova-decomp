#!/usr/bin/env python3
"""Add missing `// Ghidra 0xADDR <Name>.` citations to reimplemented functions.

Reads progress.csv, finds rows whose impl_file does not mention the row's
address, locates the port function in the impl file (exact or normalized symbol
match), and inserts the citation as the first line of the doc-comment block
immediately above the definition.

Usage:
  python3 tools/ref_citation_fixup.py            # dry-run report
  python3 tools/ref_citation_fixup.py --apply    # perform insertions
"""
import argparse
import csv
import re
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = "http://127.0.0.1:8166"
ADDR_RE = re.compile(r"0x[0-9A-Fa-f]{6,8}")


def ghidra_name(addr: str) -> str:
    try:
        with urllib.request.urlopen(f"{BASE}/function/{addr}/info", timeout=10) as r:
            parts = r.read().decode().split()
        return parts[1] if len(parts) >= 2 else "UNK"
    except Exception:  # noqa: BLE001
        return "UNK"


def normalize(name: str) -> str:
    n = name.lower().replace("_", "")
    for p in ("novamusic", "novasound", "novaui", "novarender", "novahud",
              "novaaudio", "novaprefs", "novaeffects", "nova", "asteroid"):
        if n.startswith(p):
            return n[len(p):]
    return n


def comment_prefix(comment: str) -> str | None:
    m = re.match(r"\s*([A-Za-z_][A-Za-z0-9_:]*)\s*:?", comment)
    if not m:
        return None
    s = m.group(1)
    if s.endswith(":") or ":" in s:
        s = s.split("::")[-1].rstrip(":")
    return s or None


def find_definition(lines: list[str], symbol: str) -> int | None:
    """Line index (0-based) of the definition of `symbol`, or None."""
    pat = re.compile(r"\b" + re.escape(symbol) + r"\s*\(")
    for i, line in enumerate(lines):
        if pat.search(line) and not line.rstrip().endswith(";"):
            # crude guard: skip comment lines that merely mention it
            if line.lstrip().startswith("//"):
                continue
            return i
    return None


def doc_block_start(lines: list[str], def_line: int) -> int:
    """Index of the first line of the `//` comment block above def_line."""
    i = def_line - 1
    start = def_line
    while i >= 0 and lines[i].lstrip().startswith("//"):
        start = i
        i -= 1
    return start


def word_suffix_candidates(name: str, idents: set[str]) -> list[str]:
    """Idents whose full name ends with the same CamelCase tail as `name`.

    Walks word boundaries from the left of the name, so progressively longer
    suffixes are tried (shorter tails first for tighter constraints... they
    come later in the returned list). Uniqueness is enforced by the caller.
    """
    words = re.findall(r"[A-Z][a-z0-9]*|[a-z0-9]+", name)
    out: list[str] = []
    for start in range(len(words)):
        tail = "".join(words[start:]).lower()
        if len(tail) < 12:  # too generic
            continue
        matches = [s for s in sorted(idents) if s.lower().endswith(tail)]
        if len(matches) == 1 and matches[0] not in out:
            out.append(matches[0])
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--dry-run", dest="dry_run", action="store_true")
    args = ap.parse_args()
    if args.apply and args.dry_run:
        ap.error("--apply and --dry-run are mutually exclusive")

    rows = list(csv.DictReader(open(ROOT / "progress.csv")))
    cache: dict[str, tuple[list[str], str]] = {}
    plan: list[tuple[str, int, str, str, str]] = []  # file, lineno, citation
    skipped: list[str] = []

    for r in rows:
        if not r["impl_file"]:
            continue
        path = ROOT / r["impl_file"]
        if str(path) not in cache:
            text = path.read_text()
            cache[str(path)] = (text.split("\n"), text)
        lines, text = cache[str(path)]

        addr = r["address"]
        if addr.lower() in text.lower():
            continue
        # interior/label rows handled manually
        if "INTERNAL LABEL" in r["comment"]:
            skipped.append(f"{addr} {r['name']}: INTERNAL LABEL row, manual")
            continue

        gname = ghidra_name(addr)
        # candidate symbols to try, in order
        cands: list[str] = []
        pref = comment_prefix(r["comment"])
        if pref:
            cands.append(pref)
        # known-ambiguous prefixes that grabbed the wrong definition
        bad = {(addr, s) for addr, s in (
            ("0x004B4320", "KeyBindings"),   # reset is NovaPreferences::ResetToDefaults
            ("0x004BCAD0", "NovaFontCache"),  # width method, placed manually
        )}
        cands = [c for c in cands if (addr, c) not in bad]
        # fuzzy pass over function-like identifiers found in the file
        idents = set(re.findall(r"\b([A-Za-z_]\w+)\s*\(", text))
        norm_target = normalize(r["name"])
        if norm_target:
            for ident in sorted(idents):
                if normalize(ident) == norm_target and ident not in cands:
                    cands.append(ident)
        # suffix pass: same CamelCase tail, unique in file
        for ident in word_suffix_candidates(r["name"], idents):
            if ident not in cands:
                cands.append(ident)

        placed = False
        for sym in cands:
            if sym in ("TODO", "Reimpl", "Ghidra", "INTERNAL", "NovaUi_Run"):
                continue
            dl = find_definition(lines, sym)
            if dl is None:
                continue
            insert_at = doc_block_start(lines, dl)
            citation = f"// Ghidra {addr.lower()} {gname}."
            plan.append((r["impl_file"], insert_at, citation, r["address"], sym))
            placed = True
            break
        if not placed:
            skipped.append(f"{addr} {r['name']}: no definition site found in {r['impl_file']}")

    # group per file, apply from bottom to top per file
    by_file: dict[str, list[tuple[int, str, str, str]]] = {}
    for f, ln, cit, addr, sym in plan:
        by_file.setdefault(f, []).append((ln, cit, addr, sym))

    for f, items in sorted(by_file.items()):
        items.sort(key=lambda x: -x[0])
        print(f"== {f}: {len(items)} citations")
        for ln, cit, addr, sym in items:
            print(f"  line {ln+1}: [{sym}] {cit}")
        if args.apply:
            lines = (ROOT / f).read_text().split("\n")
            for ln, cit, addr, sym in sorted(items, key=lambda x: -x[0]):
                lines.insert(ln, cit)
            (ROOT / f).write_text("\n".join(lines))

    if skipped:
        print("\n== skipped (manual):")
        for s in skipped:
            print("  " + s)


def _file_symbols(text: str):
    return set()


if __name__ == "__main__":
    main()
