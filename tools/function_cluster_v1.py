#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = [
#   "numpy>=2.3",
#   "scipy>=1.16",
# ]
# ///

"""Hierarchical subsystem clustering for Ghidra functions.

This is a pragmatic v1 aimed at decomp workflow support:
- cheap structural signals first
- sparse candidate generation to avoid O(N^2)
- hierarchical clustering inside sparse graph components

The implementation intentionally keeps the scoring knobs exposed on the CLI so
we can tune it as Ghidra metadata improves.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import html
import hashlib
import itertools
import json
import math
import os
import re
import sys
import textwrap
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Iterator, Sequence

import numpy as np
from scipy.cluster.hierarchy import dendrogram, fcluster, linkage
from scipy.sparse import csr_matrix
from scipy.sparse.csgraph import connected_components
from scipy.spatial.distance import squareform


FUNCTION_LINE_RE = re.compile(
	r"^(?P<addr>[0-9a-fA-F]{8})\s+"
	r"(?P<name>\S+)\s+"
	r"(?P<ret>.+?)\s+"
	r"(?:(?P<conv>__\w+)\s+)?"
	r"(?P<sig>\S+\(.*\))$"
)
GLOBAL_TOKEN_RE = re.compile(r"\b(?:g_[A-Za-z0-9_]+|_?DAT_[0-9A-Fa-f]+|PTR_[0-9A-Fa-f]+)\b")
ASSIGNMENT_RE = re.compile(r"(?<![=!<>])=(?!=)")
DECOMPILE_FILE_RE = re.compile(r"^(?P<addr>[0-9A-Fa-f]{8})_.*\.c$")

DEFAULT_CACHE_DIR = Path(".cache/function_clusters_v1")
DEFAULT_JOBS = min(32, (os.cpu_count() or 4) * 2)
DEFAULT_TOP_K = 16
DEFAULT_MIN_SCORE = 0.33
DEFAULT_ADDR_WINDOW = 6
DEFAULT_TAU_ADDR = 0x5000
DEFAULT_MAX_POSTING = 64
DEFAULT_DECOMPILE_BATCH = 32
DEFAULT_WEIGHT_CALL = 0.45
DEFAULT_WEIGHT_GLOBAL = 0.30
DEFAULT_WEIGHT_POS = 0.15
DEFAULT_WEIGHT_TYPE = 0.10
DEFAULT_UTILITY_DEGREE = 40
DEFAULT_CUT_SIMILARITIES = [0.75, 0.60, 0.45]


@dataclass(slots=True)
class FunctionInfo:
	addr: int
	name: str
	return_type: str
	call_conv: str
	signature: str
	params: list[str]
	addr_rank: int = 0
	callers: set[int] = field(default_factory=set)
	callees: set[int] = field(default_factory=set)
	neighbors2: set[int] = field(default_factory=set)
	globals_read: set[str] = field(default_factory=set)
	globals_write: set[str] = field(default_factory=set)
	type_shape: tuple[str, ...] = field(default_factory=tuple)
	return_shape: str = "other"

	@property
	def globals_all(self) -> set[str]:
		return self.globals_read | self.globals_write


@dataclass(slots=True)
class EdgeScore:
	left: int
	right: int
	score: float
	call_score: float
	global_score: float
	position_score: float
	type_score: float


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(
		description=(
			"Cluster Ghidra functions into likely subsystems using call-graph overlap, "
			"shared globals, signature shape, and address locality."
		),
		epilog=(
			"Examples:\n"
			"  uv run tools/function_cluster_v1.py "
			"--decompile-source folder "
			"--full-decompile-dir /tmp/ghidra_full_decompile_full "
			"--output-dir analysis/function_clusters_v1/full\n\n"
			"  uv run tools/function_cluster_v1.py "
			"--run-full-decompile "
			"--full-decompile-dir /tmp/ghidra_full_decompile_full "
			"--output-dir analysis/function_clusters_v1/full"
		),
		formatter_class=argparse.RawTextHelpFormatter,
	)
	parser.add_argument(
		"--ghidra-base",
		default="http://127.0.0.1:8166",
		help="Base URL for the local Ghidra API server.",
	)
	parser.add_argument(
		"--name-re",
		default=".*",
		help="Regex filter passed to /functions. Default: all functions.",
	)
	parser.add_argument("--limit", type=int, default=0, help="Optional function limit. Default: no limit.")
	parser.add_argument("--start", help="Optional inclusive function start address (requires --end).")
	parser.add_argument("--end", help="Optional inclusive function end address (requires --start).")
	parser.add_argument(
		"--decompile-source",
		choices=("auto", "folder", "live"),
		default="auto",
		help=(
			"Where to read decompile text from:\n"
			"  auto   prefer --full-decompile-dir, fall back to live API\n"
			"  folder require --full-decompile-dir and never hit live decompile\n"
			"  live   ignore folder files and use live API decompile"
		),
	)
	parser.add_argument(
		"--full-decompile-dir",
		type=Path,
		help="Folder created by POST /full_decompile and used for global extraction.",
	)
	parser.add_argument(
		"--run-full-decompile",
		action="store_true",
		help="Run POST /full_decompile into --full-decompile-dir before clustering.",
	)
	parser.add_argument(
		"--output-dir",
		type=Path,
		default=Path("analysis/function_clusters_v1"),
		help="Directory for JSON, TXT, and HTML reports.",
	)
	parser.add_argument(
		"--largest-component-max",
		type=int,
		default=120,
		help="Skip embedded HTML dendrogram if the largest component exceeds this size.",
	)
	return parser.parse_args()


class GhidraClient:
	def __init__(self, base: str, cache_dir: Path):
		self.base = base.rstrip("/")
		self.cache_dir = cache_dir
		self.cache_dir.mkdir(parents=True, exist_ok=True)

	def _cache_path(self, method: str, path: str, body: str | None) -> Path:
		key = f"{method} {path}\n{body or ''}".encode("utf-8")
		digest = hashlib.sha256(key).hexdigest()
		return self.cache_dir / f"{digest}.txt"

	def request(
		self,
		path: str,
		body: dict[str, object] | None = None,
		use_cache: bool = True,
		timeout: float = 120.0,
	) -> str:
		method = "POST" if body is not None else "GET"
		body_text = json.dumps(body, sort_keys=True) if body is not None else None
		cache_path = self._cache_path(method, path, body_text)
		if use_cache and cache_path.exists():
			return cache_path.read_text(encoding="utf-8")

		url = urllib.parse.urljoin(f"{self.base}/", path.lstrip("/"))
		data = body_text.encode("utf-8") if body_text is not None else None
		headers = {"Content-Type": "application/json"} if data is not None else {}
		req = urllib.request.Request(url, data=data, headers=headers, method=method)
		try:
			with urllib.request.urlopen(req, timeout=timeout) as response:
				text = response.read().decode("utf-8")
		except urllib.error.HTTPError as exc:
			detail = exc.read().decode("utf-8", errors="replace")
			raise RuntimeError(f"{method} {path} failed: HTTP {exc.code}: {detail}") from exc
		except urllib.error.URLError as exc:
			raise RuntimeError(f"{method} {path} failed: {exc.reason}") from exc

		if use_cache:
			cache_path.write_text(text, encoding="utf-8")
		return text


def chunked(values: Sequence[int], size: int) -> Iterator[Sequence[int]]:
	for index in range(0, len(values), size):
		yield values[index : index + size]


def normalize_function_listing(text: str) -> list[str]:
	lines: list[str] = []
	for raw_line in text.splitlines():
		line = raw_line.rstrip()
		if not line:
			continue
		if re.match(r"^[0-9a-fA-F]{8}\s", line):
			lines.append(line)
		elif lines:
			lines[-1] += " " + line.strip()
	return lines


def split_signature_params(signature: str) -> list[str]:
	match = re.search(r"\((.*)\)", signature)
	if not match:
		return []
	param_blob = match.group(1).strip()
	if not param_blob or param_blob == "void":
		return []
	params: list[str] = []
	current: list[str] = []
	depth = 0
	for char in param_blob:
		if char == "," and depth == 0:
			params.append("".join(current).strip())
			current = []
			continue
		if char == "(":
			depth += 1
		elif char == ")" and depth > 0:
			depth -= 1
		current.append(char)
	if current:
		params.append("".join(current).strip())
	return params


def classify_type(type_text: str) -> str:
	lower = type_text.lower()
	if "*" in type_text:
		if "func" in lower or "__cdecl" in lower or "__stdcall" in lower:
			return "fn_ptr"
		if any(token in lower for token in ("struct", "state", "class", "db", "ui", "ship", "world")):
			return "ctx_ptr"
		return "ptr"
	if any(token in lower for token in ("float", "double", "float10")):
		return "float"
	if any(token in lower for token in ("char", "short", "int", "long", "uint", "undefined", "bool")):
		return "int"
	if any(token in lower for token in ("void",)):
		return "void"
	return "other"


def parse_function_line(line: str) -> FunctionInfo:
	line = re.sub(r"\s*/\*.*\*/\s*$", "", line)
	match = FUNCTION_LINE_RE.match(line)
	if not match:
		raise ValueError(f"Could not parse function line: {line}")
	signature = match.group("sig")
	params = split_signature_params(signature)
	info = FunctionInfo(
		addr=int(match.group("addr"), 16),
		name=match.group("name"),
		return_type=match.group("ret").strip(),
		call_conv=(match.group("conv") or "").strip(),
		signature=signature.strip(),
		params=params,
	)
	info.type_shape = tuple(classify_type(param) for param in info.params)
	info.return_shape = classify_type(info.return_type)
	return info


def parse_relation_listing(text: str) -> set[int]:
	addrs: set[int] = set()
	for line in normalize_function_listing(text):
		addrs.add(int(line.split()[0], 16))
	return addrs


def parse_batch_decompile_sections(text: str) -> dict[int, str]:
	sections: dict[int, list[str]] = {}
	current_addr: int | None = None
	for line in text.splitlines():
		header = re.match(r"^==\s+([0-9A-Fa-f]{8})\s+", line)
		if header:
			current_addr = int(header.group(1), 16)
			sections[current_addr] = []
			continue
		if current_addr is not None:
			sections[current_addr].append(line)
	return {addr: "\n".join(lines).strip() for addr, lines in sections.items()}


def infer_global_accesses(decompile_text: str) -> tuple[set[str], set[str]]:
	reads: set[str] = set()
	writes: set[str] = set()
	for line in decompile_text.splitlines():
		stripped = line.strip()
		if not stripped or stripped.startswith("/*"):
			continue
		tokens = set(GLOBAL_TOKEN_RE.findall(stripped))
		if not tokens:
			continue
		reads.update(tokens)
		assign_match = ASSIGNMENT_RE.search(stripped)
		if assign_match:
			lhs = stripped[: assign_match.start()]
			lhs_tokens = set(GLOBAL_TOKEN_RE.findall(lhs))
			writes.update(lhs_tokens)
	return reads, writes


def weighted_jaccard(left: Iterable[str | int], right: Iterable[str | int], idf: dict[str | int, float]) -> float:
	left_set = set(left)
	right_set = set(right)
	if not left_set and not right_set:
		return 0.0
	union = left_set | right_set
	inter = left_set & right_set
	union_weight = sum(idf.get(token, 1.0) for token in union)
	if union_weight <= 0:
		return 0.0
	inter_weight = sum(idf.get(token, 1.0) for token in inter)
	return inter_weight / union_weight


def type_similarity(left: FunctionInfo, right: FunctionInfo) -> float:
	score = 0.0
	if len(left.params) == len(right.params):
		score += 0.2
	if left.return_shape == right.return_shape:
		score += 0.1
	paired = min(len(left.type_shape), len(right.type_shape))
	if paired:
		matches = sum(1 for index in range(paired) if left.type_shape[index] == right.type_shape[index])
		score += 0.5 * (matches / paired)
	if left.type_shape and right.type_shape and left.type_shape[0] == right.type_shape[0] == "ctx_ptr":
		score += 0.2
	return min(score, 1.0)


def call_similarity(
	left: FunctionInfo,
	right: FunctionInfo,
	idf: dict[int, float],
	utility_degree: int,
	functions: dict[int, FunctionInfo],
) -> float:
	def filtered_neighbors(values: set[int]) -> set[int]:
		return {
			addr
			for addr in values
			if len(functions[addr].callers) + len(functions[addr].callees) <= utility_degree
		}

	left_callees = filtered_neighbors(left.callees)
	right_callees = filtered_neighbors(right.callees)
	left_callers = filtered_neighbors(left.callers)
	right_callers = filtered_neighbors(right.callers)

	score = 0.0
	score += 0.4 * weighted_jaccard(left_callees, right_callees, idf)
	score += 0.4 * weighted_jaccard(left_callers, right_callers, idf)
	score += 0.2 * weighted_jaccard(left.neighbors2, right.neighbors2, idf)
	if (right.addr in left.callees) or (left.addr in right.callees):
		score += 0.05
	if left.neighbors2 and right.neighbors2 and (left.addr in right.neighbors2 or right.addr in left.neighbors2):
		score += 0.05
	return min(score, 1.0)


def position_similarity(left: FunctionInfo, right: FunctionInfo, tau_addr: float) -> float:
	return math.exp(-abs(left.addr - right.addr) / tau_addr)


def build_idf(postings: dict[str | int, set[int]], total: int) -> dict[str | int, float]:
	idf: dict[str | int, float] = {}
	for token, owners in postings.items():
		df = max(1, len(owners))
		idf[token] = math.log1p(total / df)
	return idf


def summarize_tokens(counter: Counter[str], limit: int = 6) -> list[str]:
	return [token for token, _ in counter.most_common(limit)]


def is_generic_function_name(name: str) -> bool:
	return name.startswith("FUN_") or name.startswith("thunk_")


def select_sample_addrs(addrs: list[int], limit: int) -> list[int]:
	preferred = [addr for addr in addrs if not is_generic_function_name(function_map[addr].name)]
	generic = [addr for addr in addrs if is_generic_function_name(function_map[addr].name)]
	return (preferred + generic)[:limit]


def fetch_functions(client: GhidraClient, args: argparse.Namespace) -> list[FunctionInfo]:
	query = [("name_re", args.name_re)]
	if args.limit > 0:
		query.append(("limit", str(args.limit)))
	if args.start and args.end:
		query.extend([("start", args.start), ("end", args.end)])
	path = "/functions?" + urllib.parse.urlencode(query)
	text = client.request(path)
	functions = [parse_function_line(line) for line in normalize_function_listing(text)]
	functions.sort(key=lambda func: func.addr)
	for index, func in enumerate(functions):
		func.addr_rank = index
	return functions


def fetch_call_graph(
	client: GhidraClient,
	functions: list[FunctionInfo],
	jobs: int,
) -> None:
	def worker(func: FunctionInfo) -> tuple[int, set[int]]:
		text = client.request(f"/function/0x{func.addr:08x}/callees")
		return func.addr, parse_relation_listing(text)

	with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
		for addr, callees in executor.map(worker, functions):
			func = function_map[addr]
			func.callees = {callee for callee in callees if callee in function_map and callee != addr}

	for func in functions:
		for callee in func.callees:
			function_map[callee].callers.add(func.addr)

	for func in functions:
		neighbors = set(func.callees) | set(func.callers)
		two_hop: set[int] = set()
		for neighbor_addr in neighbors:
			neighbor = function_map[neighbor_addr]
			two_hop.update(neighbor.callees)
			two_hop.update(neighbor.callers)
		two_hop.discard(func.addr)
		two_hop.difference_update(neighbors)
		func.neighbors2 = two_hop


def fetch_decomp_globals(
	client: GhidraClient,
	functions: list[FunctionInfo],
	batch_size: int,
) -> set[int]:
	addrs = [func.addr for func in functions]
	for batch in chunked(addrs, batch_size):
		text = client.request(
			"/function/decompile",
			body={"addrs": [f"0x{addr:08x}" for addr in batch]},
		)
		for addr, body in parse_batch_decompile_sections(text).items():
			if addr not in function_map:
				continue
			reads, writes = infer_global_accesses(body)
			function_map[addr].globals_read = reads
			function_map[addr].globals_write = writes
	return set(addrs)


def index_full_decompile_folder(folder: Path) -> dict[int, Path]:
	by_addr: dict[int, Path] = {}
	if not folder.exists():
		return by_addr
	for path in folder.iterdir():
		if not path.is_file():
			continue
		match = DECOMPILE_FILE_RE.match(path.name)
		if not match:
			continue
		by_addr[int(match.group("addr"), 16)] = path
	return by_addr


def fetch_decomp_globals_from_folder(folder: Path, functions: list[FunctionInfo]) -> set[int]:
	available = index_full_decompile_folder(folder)
	loaded: set[int] = set()
	for func in functions:
		path = available.get(func.addr)
		if path is None:
			continue
		reads, writes = infer_global_accesses(path.read_text(encoding="utf-8"))
		func.globals_read = reads
		func.globals_write = writes
		loaded.add(func.addr)
	return loaded


def resolve_decompile_folder(args: argparse.Namespace) -> Path | None:
	if args.full_decompile_dir is not None:
		return args.full_decompile_dir
	if args.run_full_decompile:
		return Path("/tmp/ghidra_full_decompile")
	return None


def maybe_run_full_decompile(client: GhidraClient, folder: Path | None) -> None:
	if folder is None:
		raise ValueError("--run-full-decompile requires --full-decompile-dir or the default /tmp target.")
	folder.mkdir(parents=True, exist_ok=True)
	client.request("/full_decompile", body={"folder": str(folder)}, use_cache=False, timeout=3600.0)


def add_posting_candidates(
	postings: dict[str | int, set[int]],
	candidates: dict[int, set[int]],
	max_posting: int,
) -> None:
	for owners in postings.values():
		if len(owners) < 2 or len(owners) > max_posting:
			continue
		owner_list = sorted(owners)
		for left, right in itertools.combinations(owner_list, 2):
			candidates[left].add(right)
			candidates[right].add(left)


def build_candidates(
	functions: list[FunctionInfo],
	args: argparse.Namespace,
) -> tuple[dict[int, set[int]], dict[str | int, set[int]], dict[str | int, set[int]]]:
	candidates: dict[int, set[int]] = defaultdict(set)
	global_postings: dict[str, set[int]] = defaultdict(set)
	call_feature_postings: dict[int, set[int]] = defaultdict(set)

	for index, func in enumerate(functions):
		for offset in range(1, DEFAULT_ADDR_WINDOW + 1):
			if index - offset >= 0:
				other = functions[index - offset]
				candidates[func.addr].add(other.addr)
				candidates[other.addr].add(func.addr)
			if index + offset < len(functions):
				other = functions[index + offset]
				candidates[func.addr].add(other.addr)
				candidates[other.addr].add(func.addr)

		for neighbor in func.callees | func.callers:
			candidates[func.addr].add(neighbor)
			candidates[neighbor].add(func.addr)

		for token in func.globals_all:
			global_postings[token].add(func.addr)
		for token in func.callees | func.callers | func.neighbors2:
			call_feature_postings[token].add(func.addr)

	add_posting_candidates(global_postings, candidates, DEFAULT_MAX_POSTING)
	add_posting_candidates(call_feature_postings, candidates, DEFAULT_MAX_POSTING)

	type_shape_postings: dict[tuple[str, ...], set[int]] = defaultdict(set)
	for func in functions:
		type_shape_postings[(func.return_shape, *func.type_shape)].add(func.addr)
	for owners in type_shape_postings.values():
		if len(owners) < 2 or len(owners) > DEFAULT_MAX_POSTING:
			continue
		for left, right in itertools.combinations(sorted(owners), 2):
			candidates[left].add(right)
			candidates[right].add(left)

	return candidates, global_postings, call_feature_postings


def score_edges(
	functions: list[FunctionInfo],
	candidates: dict[int, set[int]],
	global_postings: dict[str | int, set[int]],
	call_feature_postings: dict[str | int, set[int]],
	args: argparse.Namespace,
) -> list[EdgeScore]:
	global_idf = build_idf(global_postings, len(functions))
	call_idf = build_idf(call_feature_postings, len(functions))
	scored: list[EdgeScore] = []
	per_node: dict[int, list[EdgeScore]] = defaultdict(list)

	for func in functions:
		for other_addr in candidates.get(func.addr, ()):
			if other_addr <= func.addr:
				continue
			other = function_map[other_addr]
			call_score = call_similarity(func, other, call_idf, DEFAULT_UTILITY_DEGREE, function_map)
			global_score = 0.5 * weighted_jaccard(func.globals_all, other.globals_all, global_idf)
			global_score += 0.5 * weighted_jaccard(func.globals_write, other.globals_write, global_idf)
			pos_score = position_similarity(func, other, DEFAULT_TAU_ADDR)
			sig_score = type_similarity(func, other)
			score = (
				DEFAULT_WEIGHT_CALL * call_score
				+ DEFAULT_WEIGHT_GLOBAL * global_score
				+ DEFAULT_WEIGHT_POS * pos_score
				+ DEFAULT_WEIGHT_TYPE * sig_score
			)
			if score < DEFAULT_MIN_SCORE:
				continue
			edge = EdgeScore(
				left=func.addr,
				right=other.addr,
				score=score,
				call_score=call_score,
				global_score=global_score,
				position_score=pos_score,
				type_score=sig_score,
			)
			per_node[func.addr].append(edge)
			per_node[other.addr].append(edge)

	for addr, edges in per_node.items():
		edges.sort(key=lambda item: item.score, reverse=True)
		per_node[addr] = edges[: DEFAULT_TOP_K]

	kept_pairs: dict[tuple[int, int], EdgeScore] = {}
	for edges in per_node.values():
		for edge in edges:
			key = (edge.left, edge.right)
			existing = kept_pairs.get(key)
			if existing is None or edge.score > existing.score:
				kept_pairs[key] = edge
	return sorted(kept_pairs.values(), key=lambda item: item.score, reverse=True)


def build_components(functions: list[FunctionInfo], edges: list[EdgeScore]) -> list[list[int]]:
	index_of = {func.addr: index for index, func in enumerate(functions)}
	if not edges:
		return [[func.addr] for func in functions]
	rows: list[int] = []
	cols: list[int] = []
	data: list[int] = []
	for edge in edges:
		left = index_of[edge.left]
		right = index_of[edge.right]
		rows.extend([left, right])
		cols.extend([right, left])
		data.extend([1, 1])
	graph = csr_matrix((data, (rows, cols)), shape=(len(functions), len(functions)))
	component_count, labels = connected_components(graph, directed=False, return_labels=True)
	components: list[list[int]] = [[] for _ in range(component_count)]
	for func, label in zip(functions, labels, strict=True):
		components[label].append(func.addr)
	components.sort(key=len, reverse=True)
	return components


def build_component_linkage(component_addrs: list[int], edges: list[EdgeScore]) -> np.ndarray | None:
	if len(component_addrs) < 2:
		return None
	index_of = {addr: index for index, addr in enumerate(component_addrs)}
	sim = np.zeros((len(component_addrs), len(component_addrs)), dtype=np.float64)
	for edge in edges:
		if edge.left in index_of and edge.right in index_of:
			left = index_of[edge.left]
			right = index_of[edge.right]
			sim[left, right] = edge.score
			sim[right, left] = edge.score
	for index in range(len(component_addrs)):
		sim[index, index] = 1.0
	dist = 1.0 - sim
	condensed = squareform(dist, checks=False)
	return linkage(condensed, method="average")


def render_dendrogram_svg(
	component_addrs: list[int],
	linkage_matrix: np.ndarray,
) -> str:
	plot = dendrogram(
		linkage_matrix,
		labels=[f"0x{addr:08x} {function_map[addr].name}" for addr in component_addrs],
		orientation="right",
		no_plot=True,
	)
	icoords: list[list[float]] = plot["icoord"]
	dcoords: list[list[float]] = plot["dcoord"]
	labels: list[str] = plot["ivl"]
	if not icoords or not dcoords:
		return "<p>No dendrogram data.</p>"

	label_column_width = 560.0
	padding_left = label_column_width + 16.0
	padding_top = 18.0
	label_x = label_column_width - 8.0
	max_dist = max(max(row) for row in dcoords) or 1.0
	max_y = max(max(row) for row in icoords) + 10.0
	scale_x = 580.0 / max_dist
	height = int(max(240.0, max_y + 32.0))
	width = int(label_column_width + 620.0)

	def sx(value: float) -> float:
		return padding_left + value * scale_x

	def sy(value: float) -> float:
		return padding_top + value

	lines: list[str] = [
		f'<svg viewBox="0 0 {width} {height}" width="100%" height="{height}" '
		'xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Dendrogram">',
		'<rect width="100%" height="100%" fill="#fffdf8"/>',
	]
	for xs, ys in zip(dcoords, icoords, strict=True):
		points = " ".join(f"{sx(x):.2f},{sy(y):.2f}" for x, y in zip(xs, ys, strict=True))
		lines.append(
			f'<polyline points="{points}" fill="none" stroke="#1f2937" stroke-width="1.2"/>'
		)

	leaf_positions = [5.0 + 10.0 * index for index in range(len(labels))]
	for label, ypos in zip(labels, leaf_positions, strict=True):
		lines.append(
			f'<text x="{label_x}" y="{sy(ypos) + 3:.2f}" text-anchor="end" font-size="10" '
			f'font-family="monospace" fill="#111827">{html.escape(label)}</text>'
		)

	for tick in range(7):
		dist = max_dist * tick / 6.0
		x = sx(dist)
		lines.append(
			f'<line x1="{x:.2f}" y1="{padding_top - 8:.2f}" x2="{x:.2f}" '
			f'y2="{height - 12:.2f}" stroke="#e7e5e4" stroke-width="1"/>'
		)
		lines.append(
			f'<text x="{x:.2f}" y="12" text-anchor="middle" font-size="10" '
			f'font-family="monospace" fill="#78716c">{dist:.2f}</text>'
		)

	lines.append("</svg>")
	return "\n".join(lines)


def build_cluster_summaries(
	component_addrs: list[int],
	linkage_matrix: np.ndarray | None,
	cut_similarities: list[float],
) -> dict[str, list[dict[str, object]]]:
	if linkage_matrix is None:
		func = function_map[component_addrs[0]]
		return {
			"singleton": [
				{
					"functions": [func.name],
					"addresses": [f"0x{func.addr:08x}"],
					"top_globals": sorted(func.globals_all)[:6],
				}
			]
		}

	levels = {}
	for similarity in cut_similarities:
		distance = 1.0 - similarity
		labels = fcluster(linkage_matrix, t=distance, criterion="distance")
		groups: dict[int, list[int]] = defaultdict(list)
		for addr, label in zip(component_addrs, labels, strict=True):
			groups[int(label)].append(addr)
		level_name = f"sim_{similarity:.2f}"
		level_rows: list[dict[str, object]] = []
		for addrs in sorted(groups.values(), key=len, reverse=True):
			sample_addrs = select_sample_addrs(addrs, 12)
			global_counter = Counter(
				token for addr in addrs for token in function_map[addr].globals_all
			)
			callee_counter = Counter(
				function_map[target].name
				for addr in addrs
				for target in function_map[addr].callees
				if target in function_map
			)
			level_rows.append(
				{
					"size": len(addrs),
					"addresses": [f"0x{addr:08x}" for addr in addrs],
					"functions": [function_map[addr].name for addr in sample_addrs],
					"top_globals": summarize_tokens(global_counter),
					"top_callees": summarize_tokens(callee_counter),
					"address_span": [
						f"0x{min(addrs):08x}",
						f"0x{max(addrs):08x}",
					],
				}
			)
		levels[level_name] = level_rows
	return levels


def write_html_report(
	output_dir: Path,
	functions: list[FunctionInfo],
	edges: list[EdgeScore],
	components: list[list[int]],
	component_linkages: dict[int, np.ndarray | None],
	cut_similarities: list[float],
	largest_component_max: int,
) -> None:
	output_dir.mkdir(parents=True, exist_ok=True)
	largest_component = components[0] if components else []
	largest_linkage = component_linkages.get(1)
	report_components = [
		(component_id, addrs)
		for component_id, addrs in enumerate(components, start=1)
		if len(addrs) > 1
	]
	dendrogram_html = "<p>Dendrogram omitted.</p>"
	if (
		largest_component
		and largest_linkage is not None
		and len(largest_component) <= largest_component_max
	):
		dendrogram_html = render_dendrogram_svg(largest_component, largest_linkage)

	component_rows: list[str] = []
	for component_id, addrs in report_components[:100]:
		linkage_matrix = component_linkages.get(component_id)
		cuts = build_cluster_summaries(addrs, linkage_matrix, cut_similarities)
		top_globals = summarize_tokens(Counter(token for addr in addrs for token in function_map[addr].globals_all))
		sample_functions = [function_map[addr].name for addr in select_sample_addrs(addrs, 8)]
		full_functions = [f"0x{addr:08x} {function_map[addr].name}" for addr in addrs]
		cut_lines: list[str] = []
		for level_name, groups in cuts.items():
			sizes = ", ".join(str(group["size"]) for group in groups[:10] if "size" in group)
			cut_lines.append(
				f'<div><strong>{html.escape(level_name)}</strong>: {html.escape(sizes or "(none)")}</div>'
			)
		full_list_items = "".join(f"<li>{html.escape(item)}</li>" for item in full_functions)
		component_rows.append(
			"<tr>"
			f"<td>{component_id}</td>"
			f"<td>{len(addrs)}</td>"
			f"<td>0x{min(addrs):08x}..0x{max(addrs):08x}</td>"
			f"<td>{html.escape(', '.join(top_globals) or '(none)')}</td>"
			f"<td>{html.escape(', '.join(sample_functions))}</td>"
			f"<td>{''.join(cut_lines)}</td>"
			f"<td><details><summary>Show {len(addrs)} functions</summary><ol>{full_list_items}</ol></details></td>"
			"</tr>"
		)

	document = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Function Clusters</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #fffdf8;
      --fg: #1f2937;
      --muted: #6b7280;
      --line: #d6d3d1;
      --accent: #9a3412;
    }}
    body {{
      margin: 24px;
      background: var(--bg);
      color: var(--fg);
      font: 14px/1.45 ui-monospace, SFMono-Regular, Menlo, monospace;
    }}
    h1, h2 {{ margin: 0 0 12px; }}
    p {{ margin: 8px 0 16px; color: var(--muted); }}
    .stats {{ display: flex; gap: 24px; flex-wrap: wrap; margin-bottom: 24px; }}
    .stat {{ padding: 10px 12px; border: 1px solid var(--line); background: #ffffff; }}
    .stat strong {{ display: block; color: var(--accent); font-size: 20px; }}
    .panel {{ margin-top: 24px; }}
    .dendrogram {{ border: 1px solid var(--line); background: #ffffff; overflow: auto; padding: 8px; }}
    table {{ width: 100%; border-collapse: collapse; background: #ffffff; }}
    th, td {{ border: 1px solid var(--line); padding: 8px; vertical-align: top; text-align: left; }}
    th {{ background: #f5f5f4; position: sticky; top: 0; }}
    details ol {{ margin: 8px 0 0 20px; padding: 0; }}
    .note {{ font-size: 12px; }}
  </style>
</head>
<body>
  <h1>Function Clusters</h1>
  <p>HTML report for subsystem discovery from sparse hierarchical clustering.</p>
  <section class="stats">
    <div class="stat"><strong>{len(functions)}</strong>functions</div>
    <div class="stat"><strong>{len(edges)}</strong>edges</div>
    <div class="stat"><strong>{len(components)}</strong>components</div>
    <div class="stat"><strong>{len(report_components)}</strong>non-singleton components</div>
  </section>
  <section class="panel">
    <h2>Largest Component Dendrogram</h2>
    <div class="dendrogram">{dendrogram_html}</div>
    <p class="note">Only the largest component dendrogram is embedded here. Summary tables below skip singleton components.</p>
  </section>
  <section class="panel">
    <h2>Top Components</h2>
    <table>
      <thead>
        <tr>
          <th>ID</th>
          <th>Size</th>
          <th>Span</th>
          <th>Top Globals</th>
          <th>Sample Functions</th>
          <th>Cut Sizes</th>
          <th>Full Function List</th>
        </tr>
      </thead>
      <tbody>
        {''.join(component_rows)}
      </tbody>
    </table>
  </section>
</body>
</html>
"""
	(output_dir / "cluster_report.html").write_text(document, encoding="utf-8")


def write_summary(
	output_dir: Path,
	functions: list[FunctionInfo],
	edges: list[EdgeScore],
	components: list[list[int]],
	component_linkages: dict[int, np.ndarray | None],
	cut_similarities: list[float],
) -> None:
	output_dir.mkdir(parents=True, exist_ok=True)
	report_components = [
		(component_id, addrs)
		for component_id, addrs in enumerate(components, start=1)
		if len(addrs) > 1
	]
	edge_rows = [
		{
			"left": f"0x{edge.left:08x}",
			"left_name": function_map[edge.left].name,
			"right": f"0x{edge.right:08x}",
			"right_name": function_map[edge.right].name,
			"score": round(edge.score, 4),
			"call_score": round(edge.call_score, 4),
			"global_score": round(edge.global_score, 4),
			"position_score": round(edge.position_score, 4),
			"type_score": round(edge.type_score, 4),
		}
		for edge in edges[:500]
	]
	(output_dir / "top_edges.json").write_text(json.dumps(edge_rows, indent=2), encoding="utf-8")

	component_rows: list[dict[str, object]] = []
	for component_id, addrs in report_components:
		linkage_matrix = component_linkages.get(component_id)
		sample_addrs = select_sample_addrs(addrs, 16)
		global_counter = Counter(token for addr in addrs for token in function_map[addr].globals_all)
		name_counter = Counter(function_map[addr].name.split("_", 1)[0] for addr in addrs)
		component_rows.append(
			{
				"component_id": component_id,
				"size": len(addrs),
				"address_span": [f"0x{min(addrs):08x}", f"0x{max(addrs):08x}"],
				"top_prefixes": summarize_tokens(name_counter),
				"top_globals": summarize_tokens(global_counter),
				"sample_functions": [function_map[addr].name for addr in sample_addrs],
				"cuts": build_cluster_summaries(addrs, linkage_matrix, cut_similarities),
			}
		)
	(output_dir / "cluster_summary.json").write_text(
		json.dumps(
			{
				"function_count": len(functions),
				"edge_count": len(edges),
				"component_count": len(components),
				"non_singleton_component_count": len(report_components),
				"components": component_rows,
			},
			indent=2,
		),
		encoding="utf-8",
	)

	report_lines = [
		f"functions: {len(functions)}",
		f"edges_kept: {len(edges)}",
		f"components: {len(components)}",
		f"non_singleton_components: {len(report_components)}",
		"",
	]
	for row in component_rows:
		report_lines.append(
			textwrap.dedent(
				f"""\
				component {row["component_id"]}: size={row["size"]} span={row["address_span"][0]}..{row["address_span"][1]}
				  prefixes: {", ".join(row["top_prefixes"]) or "(none)"}
				  globals: {", ".join(row["top_globals"]) or "(none)"}
				  sample: {", ".join(row["sample_functions"][:8])}
				"""
			).rstrip()
		)
		report_lines.append("")
	(output_dir / "cluster_summary.txt").write_text("\n".join(report_lines), encoding="utf-8")


function_map: dict[int, FunctionInfo] = {}


def main() -> int:
	args = parse_args()
	if bool(args.start) != bool(args.end):
		print("--start and --end must be provided together.", file=sys.stderr)
		return 1
	cut_similarities = DEFAULT_CUT_SIMILARITIES
	client = GhidraClient(args.ghidra_base, DEFAULT_CACHE_DIR)
	decompile_folder = resolve_decompile_folder(args)

	print("Fetching function list...", file=sys.stderr)
	functions = fetch_functions(client, args)
	if not functions:
		print("No functions matched the requested filter.", file=sys.stderr)
		return 1

	function_map.clear()
	function_map.update({func.addr: func for func in functions})

	print(f"Fetching call graph for {len(functions)} functions...", file=sys.stderr)
	fetch_call_graph(client, functions, DEFAULT_JOBS)

	if args.run_full_decompile:
		print(f"Running full_decompile into {decompile_folder}...", file=sys.stderr)
		maybe_run_full_decompile(client, decompile_folder)

	loaded_from_folder: set[int] = set()
	if args.decompile_source in ("auto", "folder") and decompile_folder is not None:
		print(f"Loading decompile files from {decompile_folder}...", file=sys.stderr)
		loaded_from_folder = fetch_decomp_globals_from_folder(decompile_folder, functions)
		print(
			f"Loaded {len(loaded_from_folder)}/{len(functions)} functions from folder.",
			file=sys.stderr,
		)
		if args.decompile_source == "folder" and not loaded_from_folder:
			print("No matching decompile files were found in the requested folder.", file=sys.stderr)
			return 1

	if args.decompile_source == "live":
		missing = functions
	else:
		missing = [func for func in functions if func.addr not in loaded_from_folder]

	if missing and args.decompile_source != "folder":
		print(
			f"Fetching live decompile for {len(missing)} functions not covered by folder data...",
			file=sys.stderr,
		)
		fetch_decomp_globals(client, missing, DEFAULT_DECOMPILE_BATCH)
	elif missing:
		print(
			f"Proceeding without decompile text for {len(missing)} functions missing from folder mode.",
			file=sys.stderr,
		)

	print("Building sparse candidates...", file=sys.stderr)
	candidates, global_postings, call_feature_postings = build_candidates(functions, args)

	print("Scoring edges...", file=sys.stderr)
	edges = score_edges(functions, candidates, global_postings, call_feature_postings, args)

	print("Finding connected components...", file=sys.stderr)
	components = build_components(functions, edges)
	component_linkages: dict[int, np.ndarray | None] = {}
	for component_id, addrs in enumerate(components, start=1):
		component_linkages[component_id] = build_component_linkage(addrs, edges)

	output_dir: Path = args.output_dir
	print(f"Writing summaries to {output_dir}...", file=sys.stderr)
	write_summary(output_dir, functions, edges, components, component_linkages, cut_similarities)
	print(f"Writing HTML report to {output_dir / 'cluster_report.html'}...", file=sys.stderr)
	write_html_report(
		output_dir,
		functions,
		edges,
		components,
		component_linkages,
		cut_similarities,
		args.largest_component_max,
	)

	largest_component = components[0]
	largest_linkage = component_linkages.get(1)
	if (
		largest_linkage is not None
		and len(largest_component) <= args.largest_component_max
	):
		print("Embedded dendrogram written to cluster_report.html.", file=sys.stderr)
	else:
		print(
			f"Skipping embedded dendrogram for largest component size={len(largest_component)} "
			f"(limit={args.largest_component_max}).",
			file=sys.stderr,
		)

	print(
		f"Done. functions={len(functions)} edges={len(edges)} components={len(components)}",
		file=sys.stderr,
	)
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
