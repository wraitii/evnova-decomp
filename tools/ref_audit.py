#!/usr/bin/env python3
"""Cross-check progress.csv reimplementation rows against the C++ sources and
the Ghidra DB.

For each row with an impl_file:
  * query Ghidra for the current function name at the address
  * compare Ghidra name vs progress.csv `name` column
  * locate the port function in the impl file (exact, then fuzzy-normalized)
  * check whether the Ghidra address is cited anywhere in the impl file

Also cross-checks the in-source `@port` markers against the tracker:
  // @port 0xADDR[,0xADDR...] NN% [tag[,tag...]]
The marker is the authoritative port-site annotation: `NN%` is the remaining
work and the tags classify what remains or mark a decision (see PORT_TAGS).
Domain tags name unported work; status tags (`divergence`, `moddata`,
`bugfix`, `verify`) are orthogonal to `pct`, as is the structural `synthetic` tag, so a row
whose only open item is one of those is 100% and a `pct<100` row must carry a
domain tag. The audit fails on markers
with no tracker row, marker file or pct mismatches, unrecognized tags, and
permanent decisions at `pct<100`; it reports pct<100 rows with no tags and
ported rows without a marker (migration coverage, not a failure).

It also reports agreement between a marker's tag CSV and the explicit inline
markers in the port function body (up to its closing brace). Matching is
token-based; prose is never scanned, so an explanation must use one of these
exact tokens:
  * a `TODO(decomp...)` or `NovaLog::Todo(...)` deferral needs at least one tag;
  * a `divergence`/`moddata` tag needs a `DIVERGENCE(original):` or
    `BUGFIX(original)` explanation, and such an explanation needs a decision tag;
  * a `bugfix` tag needs a `BUGFIX(original)` marker, and vice-versa.
`verify` never requires an inline marker. These are migration reports, not
fatal; `synthetic` markers are skipped (their address is an interior slice, so
the next body is not necessarily the port).

Writes a report to analysis/ref_audit.txt. Read-only; no writes to the DB.

With `--gen`, rows carrying an `@port` marker have their `reimpl_pct` and
`comment` (the tag CSV) overwritten from source; all other rows stay
byte-identical.
"""
import csv
import json
import os
import re
import sys
import urllib.request
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = "http://127.0.0.1:8166"

ADDR_RE = re.compile(r"0x[0-9A-Fa-f]{6,8}")
FUNC_DEF_RE = re.compile(
    r"^(?:[A-Za-z_][\w:<>,*&\s\[\]]*?\s+)?([A-Za-z_]\w*)\s*\(", re.M
)
# Controlled vocabulary for `@port` tags. A tag names what remains in a ported
# function, so a resumer can tell a `license` gap from a `gameplay` one.
# Keep the set small; the audit rejects unknown tags.
#
# Domain tags say where unported work remains:
#   gameplay    - simulation or player-outcome behavior is missing or altered.
#                 A temporary divergence from the original is `gameplay` or
#                 `correctness`, not `divergence`.
#   rng         - visible behavior matches but the random draw count, order or
#                 seed differs, breaking replay parity with the original.
#   correctness - a known small deviation from the original (wrong constant,
#                 evaluation order, precision, guard timing); exact fix known.
#   cadence     - behavior tied to frame time or update cadence (throttles,
#                 per-frame vs staggered work, frame-count timers).
#   ui          - HUD/menu/dialog presentation with no simulation effect.
#   rendering   - sprite/world/effect drawing and its resolved frames; visual
#                 fidelity only, no simulation outcome.
#   audio       - sound or voice cue gaps.
#   license     - shareware/registration/nag paths; out of scope by design.
#
# Status tags are decisions, not work. `pct` counts remaining work, so a row
# whose only open item is a status tag is 100%; a status tag may sit beside a
# domain tag, but never replace it (a `pct<100` row needs a domain tag).
#   verify      - our value/behavior is unconfirmed and must be checked against
#                 Ghidra before it can be trusted.
#   divergence  - a deliberate difference from the original that we expect to
#                 keep permanently. A temporary divergence is `gameplay` or
#                 `correctness`, never `divergence`.
#   moddata     - divergences we make for mod-hardening, clarity, or avoiding
#                 quirky data-related behaviour where constants feel more
#                 intentional. A specialized permanent divergence.
#   bugfix      - a deliberate correction to a confirmed bug in the original
#                 executable or shipped scenario data, gated through the shared
#                 compatibility policy. Permanent; must carry an inline
#                 `BUGFIX(original)` marker.
# Structural tags describe the row, not the work:
#   synthetic   - the marker address is an interior label of a larger collapsed
#                 function (parent named in the adjacent `// Ghidra ...`
#                 comment), not a Ghidra function entry. The tracker row still
#                 records the synthesized region, but the Ghidra name and
#                 citation cross-checks do not apply.
PORT_DOMAIN_TAGS = {
    "gameplay",
    "rng",
    "license",
    "correctness",
    "cadence",
    "ui",
    "rendering",
    "audio",
}
PORT_STATUS_TAGS = {
    "verify",
    "divergence",
    "moddata",
    "bugfix",
}
PORT_STRUCTURAL_TAGS = {
    "synthetic",
}
PORT_TAGS = PORT_DOMAIN_TAGS | PORT_STATUS_TAGS | PORT_STRUCTURAL_TAGS
# Tags that mark a decided difference rather than unported work. A row whose
# only open item is one of these is 100%, so one at pct<100 without a domain
# tag is an audit failure.
PORT_DECISION_TAGS = {"divergence", "moddata", "bugfix"}
# In-source port-site marker. One comment line, one or more addresses, an
# optional percentage, and an optional comma-separated tag CSV.
PORT_RE = re.compile(
    r"^[ \t]*//[ \t]*@port[ \t]+"
    r"(?P<addrs>0x[0-9A-Fa-f]{6,8}(?:[ \t]*,[ \t]*0x[0-9A-Fa-f]{6,8})*)"
    r"(?:[ \t]+(?P<pct>\d{1,3})%)?"
    r"(?:[ \t]+(?P<tags>[A-Za-z][A-Za-z0-9_]*(?:[ \t]*,[ \t]*[A-Za-z][A-Za-z0-9_]*)*))?"
    r"[ \t]*$",
    re.M,
)
SRC_SUFFIXES = {".cpp", ".cc", ".hpp", ".hh", ".h"}
# Explicit inline markers checked against the `@port` tag CSV. Matching is
# token-based; prose is never scanned, so an explanation must be written with
# one of these exact tokens. A deferral (`TODO(decomp...)`/`NovaLog::Todo`) or
# an explanation needs at least one tag, a `divergence`/`moddata` tag needs an
# explanation, and a `bugfix` tag needs `BUGFIX(original)` and vice-versa.
TODO_MARKER_RE = re.compile(
    r"(?:TODO\s*\(\s*decomp|NovaLog\s*::\s*Todo)", re.I
)
BUGFIX_RE = re.compile(r"BUGFIX\s*\(\s*original\s*\)", re.I)
DIVERGENCE_EXPL_RE = re.compile(r"DIVERGENCE\s*\(\s*original\s*\)", re.I)


def function_body_after(text: str, start: int) -> str:
    """Return the annotated port region after an `@port` marker.

    The `@port` marker sits just above its port function, usually followed by a
    `// Ghidra ...` note that carries the divergence/TODO narrative. The scope
    therefore runs from `start` (just after the marker line) through the first
    function's closing brace, so those leading comments are included. Comments,
    chars and strings are skipped so braces in prose do not confuse the match.
    Returns "" when no body is found.
    """
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        if c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in "\"'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == "{":
            break
        i += 1
    if i >= n:
        return ""
    depth = 0
    j = i
    while j < n:
        c = text[j]
        nxt = text[j + 1] if j + 1 < n else ""
        if c == "/" and nxt == "/":
            k = text.find("\n", j)
            j = n if k < 0 else k + 1
            continue
        if c == "/" and nxt == "*":
            k = text.find("*/", j + 2)
            j = n if k < 0 else k + 2
            continue
        if c in "\"'":
            quote = c
            j += 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            j += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[start : j + 1]
        j += 1
    return ""


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


def collect_port_markers() -> tuple[dict[str, list[dict]], list[str]]:
    """Scan src/ for @port markers keyed by lowercased address.

    Returns (markers, malformed) where malformed lists lines that mention
    @port but do not parse, so a typo cannot silently drop tracking.
    """
    markers: dict[str, list[dict]] = {}
    malformed: list[str] = []
    for path in sorted((ROOT / "src").rglob("*")):
        if path.suffix not in SRC_SUFFIXES or not path.is_file():
            continue
        text = path.read_text()
        rel = path.relative_to(ROOT).as_posix()
        offset = 0
        for lineno, raw in enumerate(text.splitlines(keepends=True), start=1):
            line_end = offset + len(raw)
            if "@port" not in raw:
                offset = line_end
                continue
            m = PORT_RE.match(raw.rstrip("\r\n"))
            if m is None:
                malformed.append(f"{rel}:{lineno} {raw.strip()}")
                offset = line_end
                continue
            pct = f"{m.group('pct')}%" if m.group("pct") else ""
            tags = [t.strip().lower() for t in (m.group("tags") or "").split(",") if t.strip()]
            body = function_body_after(text, line_end)
            for addr in m.group("addrs").split(","):
                markers.setdefault(addr.strip().lower(), []).append(
                    {"file": rel, "line": lineno, "pct": pct, "tags": tags, "body": body}
                )
            offset = line_end
    return markers, malformed


def apply_markers(rows: list[dict], markers: dict[str, list[dict]]) -> int:
    """Regenerate marked rows from source; return the number changed.

    Only lines whose address carries an `@port` marker are touched, so the
    untagged backlog stays byte-identical. The first marker for an address
    owns the row; conflicting markers for one address are an audit error path.
    """
    path = ROOT / "decomp-progress.tsv"
    lines = path.read_text().splitlines()
    if not lines:
        return 0
    changed = 0
    out = [lines[0]]
    for line in lines[1:]:
        parts = line.split("\t")
        ms = markers.get(parts[0].lower()) if parts and parts[0] else None
        if not ms:
            out.append(line)
            continue
        m = ms[0]
        parts += [""] * (5 - len(parts))
        new_pct = m["pct"] or parts[3]
        new_comment = ",".join(m["tags"])
        new_line = "\t".join([parts[0], parts[1], parts[2], new_pct, new_comment])
        if new_line != line:
            changed += 1
        out.append(new_line)
    if changed:
        path.write_text("\n".join(out) + "\n")
    return changed


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

        row_tags = {t.strip() for t in r["comment"].split(",")}
        is_synthetic = "INTERNAL LABEL" in r["comment"] or "synthetic" in row_tags
        gname = ghidra_name(r["address"])
        if gname.startswith("<ERR"):
            not_in_ghidra.append(f"{r['address']} {r['name']} -> {gname}")
        elif not is_synthetic:
            csv_name = r["name"]
            # ignore case/underscore-only differences
            if normalize(gname) != normalize(csv_name):
                ghidra_mismatch.append(
                    f"{r['address']}  csv='{csv_name}'  ghidra='{gname}'"
                )

        addr = r["address"]
        if is_synthetic:
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
        if not dump_addrs:
            out.append("(decompile dump present but empty; coverage check unavailable)")
        else:
            tracked = p_addrs | s_addrs
            missing = dump_addrs - tracked - {r["address"].lower() for r in rows if "INTERNAL LABEL" in r["comment"]}
            if missing:
                out.append(f"!! functions in decompile dump without a row ({len(missing)}): {sorted(missing)[:10]}")
            else:
                out.append(f"census vs decompile dump ({len(dump_addrs)} functions): complete")
    else:
        out.append("(no decompile dump at /tmp/ghidra_full_decompile; union check skipped)")

    # --- @port marker cross-check ---------------------------------------
    markers, malformed = collect_port_markers()
    # With --gen, rewrite the marked tracker rows from source first, then audit
    # the regenerated file. This lets --gen absorb ordinary pct/tag drift
    # instead of hard-failing on it before the regeneration can run.
    if "--gen" in sys.argv:
        changed = apply_markers(rows, markers)
        rows = load_tsv(ROOT / "decomp-progress.tsv", fields)
        print(
            f"@port --gen: regenerated {changed} marked row(s) in "
            "decomp-progress.tsv"
        )
    row_by_addr = {r["address"].lower(): r for r in rows}
    unknown_marker = []       # marker address has no tracker row
    marker_file_mismatch = []  # marker file != row impl_file
    pct_mismatch = []          # marker pct != row pct
    unknown_tag = []           # tag not in PORT_TAGS
    uncharacterized = []       # pct<100 but no tags
    permanent_gap = []         # pct<100 with divergence/moddata but no domain tag
    unmarked = []              # ported row with no @port marker
    divergence_missing_marker = []  # divergence/moddata tag but no inline explanation
    bugfix_missing_marker = []      # bugfix tag but no BUGFIX(original)
    explanation_untagged = []       # inline explanation but no decision tag
    bugfix_untagged = []            # BUGFIX(original) but no bugfix tag
    divergence_untagged = []        # DIVERGENCE(original) but no divergence/moddata tag
    marker_missing_tag = []         # inline deferral marker but no tag
    no_body = []                    # marker with no scannable function body
    tag_census: Counter = Counter()
    seen = set()
    for addr, ms in sorted(markers.items()):
        row = row_by_addr.get(addr)
        for m in ms:
            loc = f"{m['file']}:{m['line']}"
            if row is None:
                unknown_marker.append(f"{addr} {loc}")
                continue
            seen.add(addr)
            if row["impl_file"] and row["impl_file"] != m["file"]:
                marker_file_mismatch.append(
                    f"{addr} marker={m['file']} row={row['impl_file']}"
                )
            if m["pct"] and m["pct"] != row["reimpl_pct"]:
                pct_mismatch.append(
                    f"{addr} marker={m['pct']} row={row['reimpl_pct']} {loc}"
                )
            for tag in m["tags"]:
                tag_census[tag] += 1
                if tag not in PORT_TAGS:
                    unknown_tag.append(f"{addr} tag='{tag}' {loc}")
            eff_pct = m["pct"] or row["reimpl_pct"]
            if eff_pct and eff_pct != "100%" and not m["tags"]:
                uncharacterized.append(f"{addr} {eff_pct} {loc}")
            if (
                eff_pct
                and eff_pct != "100%"
                and set(m["tags"]) & PORT_DECISION_TAGS
                and not set(m["tags"]) & PORT_DOMAIN_TAGS
            ):
                permanent_gap.append(f"{addr} {eff_pct} {loc}")
            # Inline-marker agreement. Synthetic markers name an interior slice
            # of a collapsed parent, so the first body after the marker is not
            # necessarily the port: skip them. Migration is not complete, so
            # these are reported, not fatal.
            tags = set(m["tags"])
            if "synthetic" in tags:
                continue
            body = m["body"]
            if not body:
                no_body.append(f"{addr} {loc}")
                continue
            has_deferral = bool(TODO_MARKER_RE.search(body))
            has_bugfix = bool(BUGFIX_RE.search(body))
            has_divergence_expl = bool(DIVERGENCE_EXPL_RE.search(body))
            has_explanation = has_bugfix or has_divergence_expl
            # tag -> inline
            if tags & {"divergence", "moddata"} and not has_explanation:
                divergence_missing_marker.append(
                    f"{addr} tags={','.join(m['tags'])} {loc}"
                )
            if "bugfix" in tags and not has_bugfix:
                bugfix_missing_marker.append(
                    f"{addr} tags={','.join(m['tags'])} {loc}"
                )
            # inline -> tag
            if has_explanation and not tags & PORT_DECISION_TAGS:
                explanation_untagged.append(
                    f"{addr} tags={','.join(m['tags']) or '-'} {loc}"
                )
            if has_bugfix and "bugfix" not in tags:
                bugfix_untagged.append(
                    f"{addr} tags={','.join(m['tags']) or '-'} {loc}"
                )
            if has_divergence_expl and not tags & {"divergence", "moddata"}:
                divergence_untagged.append(
                    f"{addr} tags={','.join(m['tags']) or '-'} {loc}"
                )
            # A deferral marker (`TODO(decomp...)`/`NovaLog::Todo`) always needs
            # at least one tag; explanations are handled above.
            if has_deferral and not tags:
                marker_missing_tag.append(f"{addr} {loc}")
    for r in rows:
        if r["impl_file"] and r["address"].lower() not in markers:
            unmarked.append(f"{r['address']} {r['name']} -> {r['impl_file']}")

    out.append(f"\nrows with impl_file: {sum(1 for r in rows if r['impl_file'])}")
    out.append(
        f"@port markers: {len(markers)} addresses ({sum(len(v) for v in markers.values())} lines); "
        f"ported rows marked: {len(seen)}"
    )
    if tag_census:
        out.append(
            "@port tag census: "
            + ", ".join(f"{t}={n}" for t, n in sorted(tag_census.items()))
        )
    out.append(f"\n== malformed @port line ({len(malformed)}) ==")
    out += malformed
    out.append(f"\n== @port marker with no progress row ({len(unknown_marker)}) ==")
    out += unknown_marker
    out.append(f"\n== @port marker file != row impl_file ({len(marker_file_mismatch)}) ==")
    out += marker_file_mismatch
    out.append(f"\n== @port pct != row pct ({len(pct_mismatch)}) ==")
    out += pct_mismatch
    out.append(f"\n== @port unrecognized tag ({len(unknown_tag)}) ==")
    out += unknown_tag
    out.append(f"\n== @port pct<100 without tags ({len(uncharacterized)}) ==")
    out += uncharacterized
    out.append(
        f"\n== @port permanent decision without a domain tag at pct<100 "
        f"({len(permanent_gap)}) =="
    )
    out += permanent_gap
    out.append(f"\n== ported rows without @port marker ({len(unmarked)}) ==")
    out += unmarked
    out.append(
        f"\n== @port divergence/moddata tag without an inline explanation "
        f"({len(divergence_missing_marker)}) =="
    )
    out += divergence_missing_marker
    out.append(
        f"\n== @port bugfix tag without a BUGFIX(original) marker "
        f"({len(bugfix_missing_marker)}) =="
    )
    out += bugfix_missing_marker
    out.append(
        f"\n== inline explanation without a decision tag "
        f"({len(explanation_untagged)}) =="
    )
    out += explanation_untagged
    out.append(
        f"\n== BUGFIX(original) without a bugfix tag ({len(bugfix_untagged)}) =="
    )
    out += bugfix_untagged
    out.append(
        f"\n== DIVERGENCE(original) without a divergence/moddata tag "
        f"({len(divergence_untagged)}) =="
    )
    out += divergence_untagged
    out.append(
        f"\n== inline deferral marker without any @port tag "
        f"({len(marker_missing_tag)}) =="
    )
    out += marker_missing_tag
    out.append(
        f"\n== @port marker with no scannable function body ({len(no_body)}) =="
    )
    out += no_body
    out.append(f"\n== Ghidra name != csv name ({len(ghidra_mismatch)}) ==")
    out += sorted(ghidra_mismatch)
    out.append(f"\n== address not cited in impl file or sibling header ({len(no_citation)}) ==")
    out += sorted(no_citation)
    out.append(f"\n== address not a function in Ghidra ({len(not_in_ghidra)}) ==")
    out += not_in_ghidra
    report = "\n".join(out)
    (ROOT / "analysis" / "ref_audit.txt").write_text(report)
    print(report[:4000])

    # Hard marker inconsistencies fail the audit; the unmarked backlog does not.
    if (
        unknown_marker
        or marker_file_mismatch
        or pct_mismatch
        or unknown_tag
        or malformed
        or permanent_gap
    ):
        sys.exit(1)


if __name__ == "__main__":
    main()
