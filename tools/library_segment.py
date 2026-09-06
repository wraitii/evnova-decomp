#!/usr/bin/env python3
"""Address-run segmentation for library/CRT triage.

The linker emits same-library objects contiguously, so a library is a
contiguous address run whose call-graph neighborhoods look alike. Adjacent
function pairs are scored by IDF-weighted neighborhood similarity (shared rare
neighbors count, shared hubs don't); boundaries are cut where similarity
collapses over a sustained stretch. Segmentation runs at several min-sim
levels and is emitted as a nested, expandable HTML report plus per-level TSVs.
Classification is done by hand from the report; nothing here is a skip decision.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import html as html_mod
import json
import math
import re
import time
import urllib.request
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

BASE = "http://127.0.0.1:8166"
DECOMP_FILE_RE = re.compile(r"^(?P<addr>[0-9A-Fa-f]{8})_(?P<name>.+)\.c$")
STRING_RE = re.compile(r'"([^"\\\n]*(?:\\.[^"\\\n]*)*)"')

# Ground-truth anchors for hand classification. String regexes fingerprint
# statically linked upstream libraries; name prefixes are hand-verified renames.
STRING_ANCHORS: list[tuple[str, re.Pattern[str]]] = [
	("libpng", re.compile(r"libpng|PNG file|IDAT|IHDR|png_(struct|read|write)|png_read_png", re.I)),
	("zlib", re.compile(r"zlib|inflate|deflate", re.I)),
	("libjpeg", re.compile(r"JPEG|JFIF|jpeg", re.I)),
	("vorbis", re.compile(r"vorbis|codebook|Xiph|ogg", re.I)),
	("crt", re.compile(r"MSL_[A-Za-z]|CodeWarrior|__amsg|__get_eh|MWRuntime")),
]
NAME_ANCHORS: list[tuple[str, re.Pattern[str]]] = [
	("libpng", re.compile(r"^PNG_|^PngHandleChunk_")),
	("libjpeg", re.compile(r"^j_|^Jpeg")),
	("vorbis", re.compile(r"^Vorbis")),
	("zlib", re.compile(r"^Zlib|^Inflate|^Deflate")),
	("crt", re.compile(r"^(MW|CString_|CStringBuffer_|thunk|Ordinal|tls_|__|WinMainCRTStartup|_onexit|_amsg)")),
]


@dataclass
class Func:
	addr: int
	name: str
	callees: set[int] = field(default_factory=set)
	callers: set[int] = field(default_factory=set)
	anchors: set[str] = field(default_factory=set)
	unnamed: bool = True


def http_get(path: str) -> str:
	for attempt in range(3):
		try:
			with urllib.request.urlopen(BASE + path, timeout=60) as resp:
				return resp.read().decode("utf-8", "replace")
		except Exception:
			if attempt == 2:
				raise
			time.sleep(1.0 * (attempt + 1))
	raise RuntimeError("unreachable")


def load_functions(decomp_dir: Path) -> list[Func]:
	funcs: list[Func] = []
	for path in sorted(decomp_dir.glob("*.c")):
		m = DECOMP_FILE_RE.match(path.name)
		if not m:
			continue
		name = m.group("name")
		text = path.read_text(errors="replace")
		f = Func(addr=int(m.group("addr"), 16), name=name, unnamed="FUN_" in name)
		for label, rx in STRING_ANCHORS:
			if any(rx.search(s) for s in STRING_RE.findall(text)):
				f.anchors.add(label)
		for label, rx in NAME_ANCHORS:
			if rx.match(name):
				f.anchors.add(label)
		funcs.append(f)
	funcs.sort(key=lambda f: f.addr)
	return funcs


def fetch_call_graph(funcs: list[Func], by_addr: dict[int, Func], jobs: int) -> None:
	def worker(f: Func) -> tuple[int, set[int]]:
		text = http_get(f"/function/0x{f.addr:08x}/callees")
		addrs = {
			int(m.group(1), 16)
			for line in text.splitlines()
			if (m := re.match(r"^([0-9a-fA-F]{8})\b", line))
		}
		return f.addr, addrs

	with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
		for addr, callees in ex.map(worker, funcs):
			by_addr[addr].callees = {c for c in callees if c in by_addr and c != addr}
	for f in funcs:
		for c in f.callees:
			by_addr[c].callers.add(f.addr)


def save_call_graph(funcs: list[Func], path: Path) -> None:
	json.dump({f"{f.addr:08x}": sorted(f.callees) for f in funcs}, open(path, "w"))


def load_call_graph(funcs: list[Func], by_addr: dict[int, Func], path: Path) -> None:
	data = json.load(open(path))
	for f in funcs:
		f.callees = {c for c in data.get(f"{f.addr:08x}", []) if c in by_addr and c != f.addr}
	for f in funcs:
		for c in f.callees:
			by_addr[c].callers.add(f.addr)


def neighbor_idf(funcs: list[Func]) -> tuple[dict[int, float], dict[int, float]]:
	"""IDF weights for callee/caller nodes: sharing a rare neighbor is strong
	evidence of same-module, sharing a hub like malloc is worthless."""
	n = len(funcs)
	df_c: Counter[int] = Counter()
	df_r: Counter[int] = Counter()
	for f in funcs:
		for c in f.callees:
			df_c[c] += 1
		for r in f.callers:
			df_r[r] += 1
	wc = {v: math.log(1 + n / d) for v, d in df_c.items()}
	wr = {v: math.log(1 + n / d) for v, d in df_r.items()}
	return wc, wr


def weighted_jaccard(a: set[int], b: set[int], w: dict[int, float]) -> float:
	if not a or not b:
		return 0.0
	inter = sum(w.get(v, 1.0) for v in a & b)
	if inter <= 0:
		return 0.0
	union = sum(w.get(v, 1.0) for v in a | b)
	return inter / union


def pair_similarity(f: Func, g: Func, wc: dict[int, float], wr: dict[int, float]) -> float:
	"""How similarly the call graph looks around two address-adjacent functions."""
	s = 0.45 * weighted_jaccard(f.callees, g.callees, wc)
	s += 0.45 * weighted_jaccard(f.callers, g.callers, wr)
	if g.addr in f.callees or f.addr in g.callees:
		s = min(1.0, s + 0.1)  # direct call between neighbors
	return s


def similarity_cuts(sims: list[float], smooth: int, min_linkless_run: int,
					min_sim: float) -> set[int]:
	"""Cut indices where adjacent-pair similarity collapses: find runs of at
	least min_linkless_run consecutive weak pairs (a pair is weak unless some
	pair within +/-smooth has similarity >= min_sim) and cut in the middle of
each run. One strong link nearby keeps a module whole; sustained weakness is
	a layout boundary."""
	n = len(sims)
	weak = [True] * n
	for i in range(n):
		for j in range(max(0, i - smooth), min(n, i + smooth + 1)):
			if sims[j] >= min_sim:
				weak[i] = False
				break
	cuts: set[int] = set()
	i = 0
	while i < n:
		if weak[i]:
			j = i
			while j < n and weak[j]:
				j += 1
			if j - i >= min_linkless_run:
				cuts.add(i + (j - i) // 2 + 1)
			i = j
		else:
			i += 1
	return cuts


def partition_by_cuts(funcs: list[Func], cuts: set[int]) -> list[list[Func]]:
	starts = [0] + sorted(cuts)
	return [funcs[a:b] for a, b in zip(starts, starts[1:] + [len(funcs)])]


def segment_rows(segments: list[list[Func]]) -> list[dict]:
	rows = []
	for k, seg in enumerate(segments):
		lo, hi = seg[0].addr, seg[-1].addr
		prefixes = Counter(f.name.split("_")[0] for f in seg if not f.unnamed)
		anchors: Counter[str] = Counter(a for f in seg for a in f.anchors)
		seg_addrs = {f.addr for f in seg}
		out_edges = sum(1 for f in seg for c in f.callees if c not in seg_addrs)
		sample = [f.name for f in seg if not f.unnamed][:4]
		rows.append({
			"segment": k,
			"addr_lo": f"0x{lo:08x}",
			"addr_hi": f"0x{hi:08x}",
			"span": hi - lo,
			"count": len(seg),
			"unnamed_pct": round(100 * sum(f.unnamed for f in seg) / len(seg)),
			"out_call_edges": out_edges,
			"anchors": ",".join(f"{a}:{c}" for a, c in anchors.most_common()) or "-",
			"top_prefixes": ",".join(f"{p}:{c}" for p, c in prefixes.most_common(5)) or "-",
			"sample_names": ", ".join(sample) or "-",
		})
	return rows


def build_tree(funcs: list[Func], levels: list[float], cutsets: list[set[int]]) -> list[dict]:
	"""Nested nodes {'seg', 'children', 'sibling_level'} from coarsest to finest.
	cutsets must be monotone (each level's cuts superset the previous). A node
	whose range has no cuts at the next level that has any cuts inside it simply
	deeper-links there, so non-splitting levels collapse away."""
	n = len(funcs)

	def split(lo: int, hi: int, li: int) -> list[dict]:
		for j in range(li, len(cutsets)):
			inner = sorted(c for c in cutsets[j] if lo < c < hi)
			if inner:
				bounds = [lo] + inner
				ranges = list(zip(bounds, bounds[1:] + [hi]))
				return [{"seg": funcs[a:b],
						 "children": split(a, b, j + 1),
						 "sibling_level": j} for a, b in ranges]
		return []

	return split(0, n, 0)


def write_html(path: Path, levels: list[float], tree: list[dict], title: str) -> None:
	def render(nodes: list[dict], depth: int) -> str:
		parts = []
		for node in nodes:
			r = segment_rows([node["seg"]])[0]
			summary = (
				f'<span class="addr">{r["addr_lo"]}&ndash;{r["addr_hi"]}</span>'
				f' n={r["count"]} unnamed={r["unnamed_pct"]}% out={r["out_call_edges"]}'
				f' <span class="anch">[{html_mod.escape(r["anchors"])}]</span>'
				f' <span class="pref">{html_mod.escape(r["top_prefixes"])}</span>'
			)
			inner = f'<div class="samples">{html_mod.escape(r["sample_names"])}</div>' \
				if r["sample_names"] != "-" else ""
			if node["children"]:
				label = f"min-sim {levels[node['children'][0]['sibling_level']]:g} splits:"
				inner += f'<div class="lvl">{label}</div>{render(node["children"], depth + 1)}'
			parts.append(
				f'<details {"open" if depth == 0 else ""}><summary>{summary}</summary>{inner}</details>'
			)
		return "\n".join(parts)

	levels_txt = ", ".join(f"{t:g}" for t in levels)
	body = render(tree, 0)
	page = f'''<!doctype html>
<html><head><meta charset="utf-8"><title>{title}</title>
<style>
body {{ font: 13px/1.5 ui-monospace, Menlo, monospace; margin: 1.5em; background: #fafafa; color: #222; }}
h1 {{ font-size: 16px; }}
.meta {{ color: #666; }}
details {{ margin-left: 1.2em; }}
summary {{ cursor: pointer; padding: 2px 4px; border-radius: 4px; }}
summary:hover {{ background: #eee; }}
.addr {{ color: #0645ad; }}
.anch {{ color: #7a3e00; }}
.pref {{ color: #2a6e2a; }}
.samples {{ color: #777; margin: 2px 0 2px 2.2em; }}
.lvl {{ color: #999; margin-left: 1.2em; font-size: 11px; }}
</style></head><body>
<h1>{title}</h1>
<p class="meta">adjacent-pair similarity cuts &mdash; levels: {levels_txt}<br>
<span class="addr">address range</span> &middot; n = functions &middot; unnamed% &middot; out = call edges leaving the run &middot;
<span class="anch">[string/name anchors]</span> &middot; <span class="pref">name prefixes</span> &middot; gray line = sample names.<br>
Anchors are evidence, not truth: name-prefix anchors (CString_, thunk_, PNG_&hellip;) are only as good as the current Ghidra renames.</p>
{body}
</body></html>'''
	path.write_text(page)


def main() -> None:
	ap = argparse.ArgumentParser(description=__doc__)
	ap.add_argument("decomp_dir", nargs="?", default="/tmp/ghidra_full_decompile")
	ap.add_argument("--out", default="analysis/library_segments")
	ap.add_argument("--jobs", type=int, default=32)
	ap.add_argument("--mode", choices=["graph", "gap"], default="graph",
					help="graph: cut where adjacent-pair graph similarity collapses; gap: cut on address gaps")
	ap.add_argument("--gap", type=lambda s: int(s, 0), default=0x400,
					help="gap mode: cut runs at address gaps larger than this (default 0x400)")
	ap.add_argument("--smooth", type=int, default=3,
					help="graph: +/- pairs to look for any linkage before calling a pair weak")
	ap.add_argument("--min-linkless-run", type=int, default=5,
					help="graph: cut inside runs of at least this many consecutive weak pairs")
	ap.add_argument("--min-sim", default="0.1,0.15,0.2,0.4",
					help="graph: comma list of weak-pair thresholds; one hierarchy level per value")
	ap.add_argument("--callgraph-cache", default="/tmp/evn_callgraph.json")
	args = ap.parse_args()

	funcs = load_functions(Path(args.decomp_dir))
	if not funcs:
		raise SystemExit("no functions parsed from decompile dir")
	by_addr = {f.addr: f for f in funcs}
	cache = Path(args.callgraph_cache)
	if cache.exists():
		print(f"loaded {len(funcs)} functions; call graph from {cache}")
		load_call_graph(funcs, by_addr, cache)
	else:
		print(f"loaded {len(funcs)} functions; fetching call graph ({args.jobs} jobs)...")
		fetch_call_graph(funcs, by_addr, args.jobs)
		save_call_graph(funcs, cache)

	out_dir = Path(args.out)
	out_dir.mkdir(parents=True, exist_ok=True)

	if args.mode == "graph":
		wc, wr = neighbor_idf(funcs)
		sims = [pair_similarity(f, g, wc, wr) for f, g in zip(funcs, funcs[1:])]
		nonzero = sorted(s for s in sims if s > 0)
		if nonzero:
			print(f"adjacent-pair similarity: {len(nonzero)}/{len(sims)} pairs linked, "
				  f"median linked sim={nonzero[len(nonzero) // 2]:.3f}")
		levels = sorted(float(x) for x in str(args.min_sim).split(","))
		# Merged cutsets: each level keeps all coarser cuts plus its new ones, so
		# every level's partition exactly refines the previous one (raw cut
		# positions can drift a couple of functions between levels). Levels whose
		# merged cutset adds nothing are collapsed away.
		levels_kept: list[float] = []
		cutsets_kept: list[set[int]] = []
		merged: set[int] = set()
		for t in levels:
			new = merged | similarity_cuts(sims, args.smooth, args.min_linkless_run, t)
			if new != merged:
				merged = new
				levels_kept.append(t)
				cutsets_kept.append(set(merged))
		if not levels_kept:
			levels_kept, cutsets_kept = [levels[0]], [set()]
		level_segs = [(t, partition_by_cuts(funcs, c))
					  for t, c in zip(levels_kept, cutsets_kept)]
	else:
		segments = [[funcs[0]]]
		for prev, f in zip(funcs, funcs[1:]):
			if f.addr - prev.addr > args.gap:
				segments.append([])
			segments[-1].append(f)
		level_segs = [(0.0, segments)]

	# Per-level TSVs; runs.tsv holds the finest level.
	for t, segs in level_segs:
		rows = segment_rows(segs)
		tag = f"{t:g}".replace(".", "_")
		name = "runs" if t == level_segs[-1][0] else f"runs_min{tag}"
		keys = list(rows[0].keys())
		with open(out_dir / f"{name}.tsv", "w") as fh:
			fh.write("\t".join(keys) + "\n")
			for r in rows:
				fh.write("\t".join(str(r[k]) for k in keys) + "\n")
		print(f"min-sim {t:g}: {len(segs)} runs -> {out_dir}/{name}.tsv")

	write_html(out_dir / "runs.html", levels_kept, build_tree(funcs, levels_kept, cutsets_kept),
			"EV Nova function runs &mdash; min-sim hierarchy")
	print(f"hierarchy -> {out_dir}/runs.html")

	rows = segment_rows(level_segs[-1][1])
	print(f"\nLargest runs (min-sim {level_segs[-1][0]:g}):")
	for r in sorted(rows, key=lambda r: -r["count"])[:25]:
		print(f"  {r['addr_lo']}-{r['addr_hi']} n={r['count']:4d} unnamed={r['unnamed_pct']:3d}% "
			  f"out_edges={r['out_call_edges']:4d} anchors[{r['anchors']}] prefixes[{r['top_prefixes']}]")
	print("\nAnchor-rich runs:")
	for r in rows:
		if r["anchors"] != "-":
			print(f"  {r['addr_lo']}-{r['addr_hi']} n={r['count']:4d} anchors[{r['anchors']}] prefixes[{r['top_prefixes']}]")


if __name__ == "__main__":
	main()
