#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = [
#   "networkx>=3.4",
# ]
# ///

"""Recursive function clustering for Ghidra full-decompile folders.

This version is intentionally opinionated:
- input is a full-decompile folder with one file per function
- live Ghidra API is used only for function metadata + call graph
- clustering is recursive community detection over a sparse similarity graph

Outputs are tuned for decomp workflow support rather than generic ML usage.
"""

from __future__ import annotations

import concurrent.futures
import html
import json
import math
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import networkx as nx
from networkx.algorithms.community import greedy_modularity_communities, kernighan_lin_bisection
from networkx.algorithms.community.quality import modularity


FUNCTION_LINE_RE = re.compile(
	r"^(?P<addr>[0-9a-fA-F]{8})\s+"
	r"(?P<name>\S+)\s+"
	r"(?P<ret>.+?)\s+"
	r"(?:(?P<conv>__\w+)\s+)?"
	r"(?P<sig>\S+\(.*\))$"
)
DECOMPILE_FILE_RE = re.compile(r"^(?P<addr>[0-9A-Fa-f]{8})_.*\.c$")
GLOBAL_TOKEN_RE = re.compile(r"\b(?:g_[A-Za-z0-9_]+|_?DAT_[0-9A-Fa-f]+|PTR_[0-9A-Fa-f]+)\b")
STRING_RE = re.compile(r'"([^"\\]*(?:\\.[^"\\]*)*)"')
ASSIGNMENT_RE = re.compile(r"(?<![=!<>])=(?!=)")

DEFAULT_GHIDRA_BASE = "http://127.0.0.1:8166"
DEFAULT_OUTPUT_DIR = Path("analysis/function_clusters_v2")
DEFAULT_JOBS = min(32, (os.cpu_count() or 4) * 2)
DEFAULT_ADDR_WINDOW = 6
DEFAULT_MAX_POSTING = 64
DEFAULT_TOP_K = 14
DEFAULT_MIN_EDGE_SCORE = 0.26
DEFAULT_UTILITY_DEGREE = 40
DEFAULT_MIN_CLUSTER_SIZE = 6
DEFAULT_MIN_CHILD_CLUSTER_SIZE = 4
DEFAULT_MAX_LEAF_SIZE = 48
DEFAULT_MAX_DEPTH = 8
DEFAULT_MIN_SPLIT_MODULARITY = 0.18

WEIGHT_CALL = 0.48
WEIGHT_GLOBAL = 0.22
WEIGHT_STRING = 0.12
WEIGHT_TYPE = 0.10
WEIGHT_POSITION = 0.08


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
	string_literals: set[str] = field(default_factory=set)
	type_shape: tuple[str, ...] = field(default_factory=tuple)
	return_shape: str = "other"

	@property
	def globals_all(self) -> set[str]:
		return self.globals_read | self.globals_write

	@property
	def is_generic_name(self) -> bool:
		return self.name.startswith("FUN_") or self.name.startswith("thunk_")


@dataclass(slots=True)
class EdgeScore:
	left: int
	right: int
	score: float
	call_score: float
	global_score: float
	string_score: float
	type_score: float
	position_score: float


@dataclass(slots=True)
class ClusterNode:
	node_id: int
	addrs: list[int]
	depth: int
	score: float
	children: list["ClusterNode"] = field(default_factory=list)

	@property
	def is_leaf(self) -> bool:
		return not self.children


function_map: dict[int, FunctionInfo] = {}


class GhidraClient:
	def __init__(self, base: str):
		self.base = base.rstrip("/")
		self.cache: dict[str, str] = {}

	def request(self, path: str, *, use_cache: bool = True, timeout: float = 120.0) -> str:
		if use_cache and path in self.cache:
			return self.cache[path]

		url = urllib.parse.urljoin(f"{self.base}/", path.lstrip("/"))
		req = urllib.request.Request(url, method="GET")
		try:
			with urllib.request.urlopen(req, timeout=timeout) as response:
				text = response.read().decode("utf-8")
		except urllib.error.HTTPError as exc:
			detail = exc.read().decode("utf-8", errors="replace")
			raise RuntimeError(f"GET {path} failed: HTTP {exc.code}: {detail}") from exc
		except urllib.error.URLError as exc:
			raise RuntimeError(f"GET {path} failed: {exc.reason}") from exc

		if use_cache:
			self.cache[path] = text
		return text


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
	if "void" in lower:
		return "void"
	return "other"


def parse_function_line(line: str) -> FunctionInfo:
	line = re.sub(r"\s*/\*.*\*/\s*$", "", line)
	match = FUNCTION_LINE_RE.match(line)
	if not match:
		raise ValueError(f"Could not parse function line: {line}")
	signature = match.group("sig")
	params = split_signature_params(signature)
	func = FunctionInfo(
		addr=int(match.group("addr"), 16),
		name=match.group("name"),
		return_type=match.group("ret").strip(),
		call_conv=(match.group("conv") or "").strip(),
		signature=signature.strip(),
		params=params,
	)
	func.type_shape = tuple(classify_type(param) for param in params)
	func.return_shape = classify_type(func.return_type)
	return func


def parse_relation_listing(text: str) -> set[int]:
	addrs: set[int] = set()
	for line in normalize_function_listing(text):
		addrs.add(int(line.split()[0], 16))
	return addrs


def weighted_jaccard(left: Iterable[str | int], right: Iterable[str | int], idf: dict[str | int, float]) -> float:
	left_set = set(left)
	right_set = set(right)
	if not left_set and not right_set:
		return 0.0
	union = left_set | right_set
	inter = left_set & right_set
	union_weight = sum(idf.get(token, 1.0) for token in union)
	if union_weight <= 0.0:
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
) -> float:
	def filtered_neighbors(values: set[int]) -> set[int]:
		return {
			addr
			for addr in values
			if addr in function_map and len(function_map[addr].callers) + len(function_map[addr].callees) <= utility_degree
		}

	left_callees = filtered_neighbors(left.callees)
	right_callees = filtered_neighbors(right.callees)
	left_callers = filtered_neighbors(left.callers)
	right_callers = filtered_neighbors(right.callers)
	score = 0.0
	score += 0.42 * weighted_jaccard(left_callees, right_callees, idf)
	score += 0.42 * weighted_jaccard(left_callers, right_callers, idf)
	score += 0.16 * weighted_jaccard(left.neighbors2, right.neighbors2, idf)
	if right.addr in left.callees or left.addr in right.callees:
		score += 0.05
	if left.addr in right.callers or right.addr in left.callers:
		score += 0.03
	return min(score, 1.0)


def position_similarity(left: FunctionInfo, right: FunctionInfo) -> float:
	return math.exp(-abs(left.addr - right.addr) / 0x5000)


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
			writes.update(GLOBAL_TOKEN_RE.findall(lhs))
	return reads, writes


def infer_string_literals(decompile_text: str) -> set[str]:
	values: set[str] = set()
	for match in STRING_RE.findall(decompile_text):
		value = bytes(match, "utf-8").decode("unicode_escape", errors="ignore").strip()
		if len(value) < 4:
			continue
		if len(value) > 80:
			value = value[:80]
		values.add(value)
	return values


def build_idf(postings: dict[str | int, set[int]], total: int) -> dict[str | int, float]:
	idf: dict[str | int, float] = {}
	for token, owners in postings.items():
		df = max(1, len(owners))
		idf[token] = math.log1p(total / df)
	return idf


def summarize_tokens(counter: Counter[str], limit: int = 6) -> list[str]:
	return [token for token, _ in counter.most_common(limit)]


def is_named_global(token: str) -> bool:
	return not (
		token.startswith("DAT_")
		or token.startswith("_DAT_")
		or token.startswith("PTR_")
	)


def select_sample_addrs(addrs: list[int], limit: int) -> list[int]:
	preferred = [addr for addr in addrs if not function_map[addr].is_generic_name]
	return preferred[:limit]


def parse_full_decompile_folder(folder: Path) -> dict[int, Path]:
	if not folder.is_dir():
		raise ValueError(f"Full-decompile folder does not exist: {folder}")
	paths: dict[int, Path] = {}
	for path in sorted(folder.iterdir()):
		if not path.is_file():
			continue
		match = DECOMPILE_FILE_RE.match(path.name)
		if match is None:
			continue
		paths[int(match.group("addr"), 16)] = path
	if not paths:
		raise ValueError(f"No function decompile files found in {folder}")
	return paths


def fetch_functions(client: GhidraClient) -> list[FunctionInfo]:
	text = client.request("/functions?name_re=.*")
	functions = [parse_function_line(line) for line in normalize_function_listing(text)]
	functions.sort(key=lambda func: func.addr)
	for index, func in enumerate(functions):
		func.addr_rank = index
	return functions


def fetch_call_graph(client: GhidraClient, functions: list[FunctionInfo]) -> None:
	def worker(func: FunctionInfo) -> tuple[int, set[int]]:
		text = client.request(f"/function/0x{func.addr:08x}/callees")
		return func.addr, parse_relation_listing(text)

	with concurrent.futures.ThreadPoolExecutor(max_workers=DEFAULT_JOBS) as executor:
		for addr, callees in executor.map(worker, functions):
			func = function_map[addr]
			func.callees = {callee for callee in callees if callee in function_map and callee != addr}

	for func in functions:
		for callee in func.callees:
			function_map[callee].callers.add(func.addr)

	for func in functions:
		neighbors = func.callees | func.callers
		two_hop: set[int] = set()
		for neighbor_addr in neighbors:
			neighbor = function_map[neighbor_addr]
			two_hop.update(neighbor.callees)
			two_hop.update(neighbor.callers)
		two_hop.discard(func.addr)
		two_hop.difference_update(neighbors)
		func.neighbors2 = two_hop


def load_decompile_features(file_map: dict[int, Path], functions: list[FunctionInfo]) -> None:
	for func in functions:
		path = file_map.get(func.addr)
		if path is None:
			continue
		text = path.read_text(encoding="utf-8", errors="replace")
		func.globals_read, func.globals_write = infer_global_accesses(text)
		func.string_literals = infer_string_literals(text)


def build_candidates(
	functions: list[FunctionInfo],
) -> tuple[dict[int, set[int]], dict[str, set[int]], dict[str, set[int]], dict[int, set[int]]]:
	candidates: dict[int, set[int]] = defaultdict(set)
	global_postings: dict[str, set[int]] = defaultdict(set)
	string_postings: dict[str, set[int]] = defaultdict(set)
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
		for token in func.string_literals:
			string_postings[token].add(func.addr)
		for token in func.callees | func.callers | func.neighbors2:
			call_feature_postings[token].add(func.addr)

	for postings in (global_postings, string_postings, call_feature_postings):
		for owners in postings.values():
			if len(owners) < 2 or len(owners) > DEFAULT_MAX_POSTING:
				continue
			owner_list = sorted(owners)
			for left_index, left in enumerate(owner_list):
				for right in owner_list[left_index + 1 :]:
					candidates[left].add(right)
					candidates[right].add(left)

	type_shape_postings: dict[tuple[str, ...], set[int]] = defaultdict(set)
	for func in functions:
		type_shape_postings[(func.return_shape, *func.type_shape)].add(func.addr)
	for owners in type_shape_postings.values():
		if len(owners) < 2 or len(owners) > DEFAULT_MAX_POSTING:
			continue
		owner_list = sorted(owners)
		for left_index, left in enumerate(owner_list):
			for right in owner_list[left_index + 1 :]:
				candidates[left].add(right)
				candidates[right].add(left)

	return candidates, global_postings, string_postings, call_feature_postings


def score_edges(
	functions: list[FunctionInfo],
	candidates: dict[int, set[int]],
	global_postings: dict[str, set[int]],
	string_postings: dict[str, set[int]],
	call_feature_postings: dict[int, set[int]],
) -> list[EdgeScore]:
	global_idf = build_idf(global_postings, len(functions))
	string_idf = build_idf(string_postings, len(functions))
	call_idf = build_idf(call_feature_postings, len(functions))
	per_node: dict[int, list[EdgeScore]] = defaultdict(list)

	for func in functions:
		for other_addr in candidates.get(func.addr, ()):
			if other_addr <= func.addr:
				continue
			other = function_map[other_addr]
			call_score = call_similarity(func, other, call_idf, DEFAULT_UTILITY_DEGREE)
			global_score = 0.40 * weighted_jaccard(func.globals_all, other.globals_all, global_idf)
			global_score += 0.60 * weighted_jaccard(func.globals_write, other.globals_write, global_idf)
			string_score = weighted_jaccard(func.string_literals, other.string_literals, string_idf)
			type_score = type_similarity(func, other)
			pos_score = position_similarity(func, other)
			score = (
				WEIGHT_CALL * call_score
				+ WEIGHT_GLOBAL * global_score
				+ WEIGHT_STRING * string_score
				+ WEIGHT_TYPE * type_score
				+ WEIGHT_POSITION * pos_score
			)
			if score < DEFAULT_MIN_EDGE_SCORE:
				continue
			edge = EdgeScore(
				left=func.addr,
				right=other_addr,
				score=score,
				call_score=call_score,
				global_score=global_score,
				string_score=string_score,
				type_score=type_score,
				position_score=pos_score,
			)
			per_node[func.addr].append(edge)
			per_node[other_addr].append(edge)

	kept_pairs: dict[tuple[int, int], EdgeScore] = {}
	for edges in per_node.values():
		edges.sort(key=lambda item: item.score, reverse=True)
		for edge in edges[:DEFAULT_TOP_K]:
			key = (edge.left, edge.right)
			existing = kept_pairs.get(key)
			if existing is None or edge.score > existing.score:
				kept_pairs[key] = edge
	return sorted(kept_pairs.values(), key=lambda item: item.score, reverse=True)


def build_similarity_graph(functions: list[FunctionInfo], edges: list[EdgeScore]) -> nx.Graph:
	graph = nx.Graph()
	for func in functions:
		graph.add_node(func.addr)
	for edge in edges:
		graph.add_edge(
			edge.left,
			edge.right,
			weight=edge.score,
			call_score=edge.call_score,
			global_score=edge.global_score,
			string_score=edge.string_score,
			type_score=edge.type_score,
			position_score=edge.position_score,
		)
	return graph


def next_node_id(counter: list[int]) -> int:
	counter[0] += 1
	return counter[0]


def sort_cluster_addrs(graph: nx.Graph, addrs: Iterable[int]) -> list[int]:
	return sorted(addrs, key=lambda addr: (function_map[addr].addr_rank, -graph.degree(addr, weight="weight")))


def normalize_partition(partition: Iterable[set[int] | frozenset[int]]) -> list[set[int]]:
	groups = [set(group) for group in partition if group]
	if len(groups) <= 1:
		return groups

	small_groups = [group for group in groups if len(group) < DEFAULT_MIN_CHILD_CLUSTER_SIZE]
	large_groups = [group for group in groups if len(group) >= DEFAULT_MIN_CHILD_CLUSTER_SIZE]
	if small_groups:
		merged_small_group: set[int] = set()
		for group in small_groups:
			merged_small_group.update(group)
		if merged_small_group:
			large_groups.append(merged_small_group)

	large_groups.sort(key=lambda group: min(group))
	return large_groups


def find_partition(
	subgraph: nx.Graph,
	addrs: list[int],
) -> tuple[list[set[int]], float]:
	partition = normalize_partition(greedy_modularity_communities(subgraph, weight="weight"))
	if len(partition) > 1:
		split_modularity = modularity(subgraph, partition, weight="weight") if subgraph.number_of_edges() else 0.0
		largest_ratio = max(len(group) for group in partition) / len(addrs)
		if split_modularity >= DEFAULT_MIN_SPLIT_MODULARITY and not (
			largest_ratio > 0.90 and len(partition) == 2
		):
			return partition, split_modularity

	if len(addrs) <= DEFAULT_MAX_LEAF_SIZE:
		return [], 0.0

	if len(addrs) < DEFAULT_MIN_CHILD_CLUSTER_SIZE * 2:
		return [], 0.0

	sorted_addrs = sort_cluster_addrs(subgraph, addrs)
	midpoint = len(sorted_addrs) // 2
	initial_partition = (set(sorted_addrs[:midpoint]), set(sorted_addrs[midpoint:]))
	if not initial_partition[0] or not initial_partition[1]:
		return [], 0.0

	# TODO: Balanced bisection may diverge from the modularity optimum. We do this
	# intentionally for rename workflow usability so very large residual leaves keep splitting.
	left_group, right_group = kernighan_lin_bisection(
		subgraph,
		partition=initial_partition,
		weight="weight",
		seed=0,
	)
	forced_partition = normalize_partition((left_group, right_group))
	if len(forced_partition) <= 1:
		return [], 0.0
	split_modularity = modularity(subgraph, forced_partition, weight="weight") if subgraph.number_of_edges() else 0.0
	return forced_partition, split_modularity


def recursive_partition(
	graph: nx.Graph,
	addrs: list[int],
	depth: int,
	node_counter: list[int],
) -> ClusterNode:
	subgraph = graph.subgraph(addrs).copy()
	node = ClusterNode(
		node_id=next_node_id(node_counter),
		addrs=sort_cluster_addrs(graph, addrs),
		depth=depth,
		score=0.0,
	)
	if (
		depth >= DEFAULT_MAX_DEPTH
		or len(addrs) < DEFAULT_MIN_CLUSTER_SIZE * 2
		or len(addrs) <= DEFAULT_MAX_LEAF_SIZE and subgraph.number_of_edges() < len(addrs) * 2
	):
		return node

	partition, split_modularity = find_partition(subgraph, addrs)
	if len(partition) <= 1:
		return node

	node.score = split_modularity
	for group in partition:
		child_addrs = sorted(group)
		if len(child_addrs) < DEFAULT_MIN_CLUSTER_SIZE:
			node.children.append(
				ClusterNode(
					node_id=next_node_id(node_counter),
					addrs=sort_cluster_addrs(graph, child_addrs),
					depth=depth + 1,
					score=0.0,
				)
			)
			continue
		node.children.append(recursive_partition(graph, child_addrs, depth + 1, node_counter))

	if len(node.children) < 2:
		node.children.clear()
		node.score = 0.0
		return node
	return node


def flatten_leaves(node: ClusterNode) -> list[ClusterNode]:
	if node.is_leaf:
		return [node]
	leaves: list[ClusterNode] = []
	for child in node.children:
		leaves.extend(flatten_leaves(child))
	return leaves


def cluster_name_prefixes(addrs: list[int]) -> list[str]:
	counter = Counter()
	for addr in addrs:
		name = function_map[addr].name
		if function_map[addr].is_generic_name:
			continue
		counter[name.split("_", 1)[0]] += 1
	return summarize_tokens(counter, limit=4)


def cluster_summary(addrs: list[int]) -> dict[str, object]:
	global_counter = Counter(
		token
		for addr in addrs
		for token in function_map[addr].globals_all
		if is_named_global(token)
	)
	string_counter = Counter(token for addr in addrs for token in function_map[addr].string_literals)
	callee_counter = Counter(
		function_map[target].name
		for addr in addrs
		for target in function_map[addr].callees
		if target in function_map
	)
	generic_count = sum(1 for addr in addrs if function_map[addr].is_generic_name)
	return {
		"size": len(addrs),
		"generic_count": generic_count,
		"generic_ratio": round(generic_count / len(addrs), 3),
		"unnamed_count": generic_count,
		"unnamed_ratio": round(generic_count / len(addrs), 3),
		"address_span": [f"0x{min(addrs):08x}", f"0x{max(addrs):08x}"],
		"prefixes": cluster_name_prefixes(addrs),
		"top_globals": summarize_tokens(global_counter),
		"top_strings": summarize_tokens(string_counter),
		"top_callees": summarize_tokens(callee_counter),
		"sample_functions": [function_map[addr].name for addr in select_sample_addrs(addrs, 4)],
	}


def cluster_to_dict(node: ClusterNode) -> dict[str, object]:
	payload: dict[str, object] = {
		"node_id": node.node_id,
		"depth": node.depth,
		"split_score": round(node.score, 4),
		"summary": cluster_summary(node.addrs),
		"addresses": [f"0x{addr:08x}" for addr in node.addrs],
	}
	if node.children:
		payload["children"] = [child.node_id for child in node.children]
	return payload


def cluster_to_tree(node: ClusterNode) -> dict[str, object]:
	payload: dict[str, object] = {
		"node_id": node.node_id,
		"depth": node.depth,
		"split_score": round(node.score, 4),
		"summary": cluster_summary(node.addrs),
		"addresses": [f"0x{addr:08x}" for addr in node.addrs],
		"children": [cluster_to_tree(child) for child in node.children],
	}
	return payload


def build_cluster_index(node: ClusterNode) -> dict[str, dict[str, object]]:
	index = {str(node.node_id): cluster_to_dict(node)}
	for child in node.children:
		index.update(build_cluster_index(child))
	return index


def build_address_name_index() -> dict[str, str]:
	return {
		f"0x{addr:08x}": function_map[addr].name
		for addr in sorted(function_map)
	}


def flatten_clusters(node: ClusterNode) -> list[ClusterNode]:
	nodes = [node]
	for child in node.children:
		nodes.extend(flatten_clusters(child))
	return nodes


def timeline_prefix_color(prefixes: list[str]) -> str:
	label = prefixes[0] if prefixes else "(none)"
	if label == "(none)":
		return "#94a3b8"
	hash_value = sum((index + 1) * ord(char) for index, char in enumerate(label))
	hue = hash_value % 360
	return f"hsl({hue} 58% 60%)"


def render_cluster_timeline_svg(root: ClusterNode) -> str:
	nodes = flatten_clusters(root)
	if not nodes:
		return "<p>No clusters to render.</p>"

	summaries = {node.node_id: cluster_summary(node.addrs) for node in nodes}
	min_addr = min(int(summaries[node.node_id]["address_span"][0], 16) for node in nodes)
	max_addr = max(int(summaries[node.node_id]["address_span"][1], 16) for node in nodes)
	addr_span = max(1, max_addr - min_addr)
	max_depth = max(node.depth for node in nodes)

	plot_width = 1600
	lane_height = 18
	depth_gap = 14
	header_height = 24
	label_width = 80

	depth_nodes: dict[int, list[ClusterNode]] = defaultdict(list)
	for node in nodes:
		depth_nodes[node.depth].append(node)

	depth_layouts: dict[int, list[tuple[ClusterNode, int, int]]] = {}
	depth_lane_counts: dict[int, int] = {}
	depth_y_offsets: dict[int, int] = {}

	current_y = header_height
	for depth in range(max_depth + 1):
		entries: list[tuple[int, int, ClusterNode]] = []
		for node in sorted(depth_nodes.get(depth, []), key=lambda item: (len(item.addrs), item.node_id), reverse=True):
			summary = summaries[node.node_id]
			start_addr = int(summary["address_span"][0], 16)
			end_addr = int(summary["address_span"][1], 16)
			entries.append((start_addr, end_addr, node))

		lane_ends: list[int] = []
		layout: list[tuple[ClusterNode, int, int]] = []
		for start_addr, end_addr, node in sorted(entries, key=lambda item: (item[0], item[1], item[2].node_id)):
			lane_index = 0
			while lane_index < len(lane_ends) and start_addr <= lane_ends[lane_index]:
				lane_index += 1
			if lane_index == len(lane_ends):
				lane_ends.append(end_addr)
			else:
				lane_ends[lane_index] = end_addr
			layout.append((node, start_addr, end_addr))

		depth_layouts[depth] = layout
		depth_lane_counts[depth] = max(1, len(lane_ends))
		depth_y_offsets[depth] = current_y
		current_y += depth_lane_counts[depth] * lane_height + depth_gap

	total_height = current_y + 8

	parts = [
		f'<svg class="timeline-svg" viewBox="0 0 {label_width + plot_width} {total_height}" '
		f'role="img" aria-label="Cluster timeline by address and tree depth">'
	]

	for depth in range(max_depth + 1):
		base_y = depth_y_offsets[depth]
		lane_count = depth_lane_counts[depth]
		band_height = lane_count * lane_height
		parts.append(
			f'<text x="8" y="{base_y + 12}" class="timeline-label">d{depth}</text>'
			f'<line x1="{label_width}" y1="{base_y + band_height + 2}" '
			f'x2="{label_width + plot_width}" y2="{base_y + band_height + 2}" class="timeline-grid" />'
		)
		for lane_index in range(lane_count):
			lane_y = base_y + lane_index * lane_height
			parts.append(
				f'<line x1="{label_width}" y1="{lane_y + lane_height - 3}" '
				f'x2="{label_width + plot_width}" y2="{lane_y + lane_height - 3}" class="timeline-lane" />'
			)

	tick_count = 8
	for tick_index in range(tick_count + 1):
		ratio = tick_index / tick_count
		addr = min_addr + int(addr_span * ratio)
		x = label_width + ratio * plot_width
		parts.append(
			f'<line x1="{x:.2f}" y1="18" x2="{x:.2f}" y2="{total_height - 4}" class="timeline-axis" />'
			f'<text x="{x:.2f}" y="14" class="timeline-tick" text-anchor="middle">0x{addr:08x}</text>'
		)

	for depth in range(max_depth + 1):
		lane_ends: list[int] = []
		for node, start_addr, end_addr in sorted(depth_layouts[depth], key=lambda item: (item[1], item[2], item[0].node_id)):
			lane_index = 0
			while lane_index < len(lane_ends) and start_addr <= lane_ends[lane_index]:
				lane_index += 1
			if lane_index == len(lane_ends):
				lane_ends.append(end_addr)
			else:
				lane_ends[lane_index] = end_addr

			summary = summaries[node.node_id]
			x = label_width + ((start_addr - min_addr) / addr_span) * plot_width
			width = max(2.0, ((end_addr - start_addr) / addr_span) * plot_width)
			y = depth_y_offsets[depth] + lane_index * lane_height
			fill = timeline_prefix_color(summary["prefixes"])
			stroke_width = 1.6 if node.is_leaf else 1.0
			opacity = 0.88 if node.is_leaf else 0.52
			prefix_text = ", ".join(summary["prefixes"]) or "(none)"
			sample_text = ", ".join(summary["sample_functions"][:3]) or "(none)"
			tooltip = html.escape(
				f"cluster {node.node_id} | depth {node.depth} | size {summary['size']} | "
				f"span {summary['address_span'][0]}..{summary['address_span'][1]} | "
				f"unnamed {summary['generic_count']}/{summary['size']} | "
				f"prefixes {prefix_text} | samples {sample_text}"
			)
			parts.append(
				f'<rect x="{x:.2f}" y="{y + 1}" width="{width:.2f}" height="12" rx="3" ry="3" '
				f'fill="{fill}" fill-opacity="{opacity:.2f}" stroke="{fill}" '
				f'stroke-width="{stroke_width:.1f}" class="timeline-cluster">'
				f"<title>{tooltip}</title>"
				f"</rect>"
			)

	parts.append("</svg>")
	return "".join(parts)


def render_tree_lines(node: ClusterNode) -> list[str]:
	summary = cluster_summary(node.addrs)
	lines = [
		(
			f'{"  " * node.depth}- cluster {node.node_id}: '
			f'size={summary["size"]} span={summary["address_span"][0]}..{summary["address_span"][1]} '
			f'unnamed={summary["generic_count"]}/{summary["size"]} '
			f'prefixes={", ".join(summary["prefixes"]) or "(none)"} '
			f'split={node.score:.3f}'
		)
	]
	if summary["top_globals"]:
		lines.append(f'{"  " * node.depth}  globals: {", ".join(summary["top_globals"])}')
	if summary["sample_functions"]:
		lines.append(f'{"  " * node.depth}  sample: {", ".join(summary["sample_functions"])}')
	for child in node.children:
		lines.extend(render_tree_lines(child))
	return lines


def build_leaf_membership(root: ClusterNode) -> dict[int, int]:
	membership: dict[int, int] = {}
	for leaf in flatten_leaves(root):
		for addr in leaf.addrs:
			membership[addr] = leaf.node_id
	return membership


def mine_address_spans(functions: list[FunctionInfo], membership: dict[int, int]) -> list[dict[str, object]]:
	ordered = sorted(functions, key=lambda func: func.addr)
	spans: list[list[int]] = []
	current: list[int] = []
	current_leaf: int | None = None
	last_addr: int | None = None

	for func in ordered:
		leaf = membership.get(func.addr, -1)
		contiguous_enough = last_addr is None or func.addr - last_addr <= 0x400
		if current and (leaf != current_leaf or not contiguous_enough):
			spans.append(current)
			current = []
		current.append(func.addr)
		current_leaf = leaf
		last_addr = func.addr
	if current:
		spans.append(current)

	rows: list[dict[str, object]] = []
	for addrs in spans:
		if len(addrs) < 4:
			continue
		summary = cluster_summary(addrs)
		if summary["generic_ratio"] < 0.35 and len(addrs) < 8:
			continue
		rows.append(
			{
				"leaf_id": membership.get(addrs[0], -1),
				"size": len(addrs),
				"address_span": summary["address_span"],
				"generic_ratio": summary["generic_ratio"],
				"prefixes": summary["prefixes"],
				"top_globals": summary["top_globals"],
				"sample_functions": summary["sample_functions"],
			}
		)

	rows.sort(key=lambda row: (row["generic_ratio"], row["size"]), reverse=True)
	return rows


def write_outputs(
	output_dir: Path,
	root: ClusterNode,
	edges: list[EdgeScore],
	spans: list[dict[str, object]],
) -> None:
	output_dir.mkdir(parents=True, exist_ok=True)
	cluster_index = build_cluster_index(root)
	tree_payload = {
		"root_id": root.node_id,
		"root": cluster_to_tree(root),
		"clusters_by_id": cluster_index,
		"clusters": cluster_index,
		"address_to_name": build_address_name_index(),
	}
	(output_dir / "cluster_tree.json").write_text(json.dumps(tree_payload, indent=2), encoding="utf-8")
	(output_dir / "top_edges.json").write_text(
		json.dumps(
			[
				{
					"left": f"0x{edge.left:08x}",
					"left_name": function_map[edge.left].name,
					"right": f"0x{edge.right:08x}",
					"right_name": function_map[edge.right].name,
					"score": round(edge.score, 4),
					"call_score": round(edge.call_score, 4),
					"global_score": round(edge.global_score, 4),
					"string_score": round(edge.string_score, 4),
					"type_score": round(edge.type_score, 4),
					"position_score": round(edge.position_score, 4),
				}
				for edge in edges[:500]
			],
			indent=2,
		),
		encoding="utf-8",
	)
	(output_dir / "rename_spans.json").write_text(json.dumps(spans, indent=2), encoding="utf-8")
	(output_dir / "cluster_tree.txt").write_text("\n".join(render_tree_lines(root)) + "\n", encoding="utf-8")

	span_rows = []
	for span in spans[:80]:
		span_rows.append(
			"<tr>"
			f"<td>{html.escape(str(span['leaf_id']))}</td>"
			f"<td>{html.escape(str(span['size']))}</td>"
			f"<td>{html.escape(span['address_span'][0])}..{html.escape(span['address_span'][1])}</td>"
			f"<td>{html.escape(str(span['generic_ratio']))}</td>"
			f"<td>{html.escape(', '.join(span['prefixes']) or '(none)')}</td>"
			f"<td>{html.escape(', '.join(span['top_globals']) or '(none)')}</td>"
			f"<td>{html.escape(', '.join(span['sample_functions']) or '(none)')}</td>"
			"</tr>"
		)

	tree_text = html.escape((output_dir / "cluster_tree.txt").read_text(encoding="utf-8"))
	timeline_svg = render_cluster_timeline_svg(root)
	document = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>Function Clusters V2</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #fbfaf5;
      --fg: #1f2937;
      --muted: #6b7280;
      --line: #d6d3d1;
      --accent: #92400e;
    }}
    body {{
      margin: 24px;
      background: var(--bg);
      color: var(--fg);
      font: 14px/1.45 ui-monospace, SFMono-Regular, Menlo, monospace;
    }}
    h1, h2 {{ margin: 0 0 12px; }}
    p {{ margin: 8px 0 16px; color: var(--muted); }}
    section {{ margin-bottom: 28px; }}
    .stats {{ display: flex; gap: 24px; flex-wrap: wrap; margin-bottom: 24px; }}
    .stat {{ padding: 10px 12px; border: 1px solid var(--line); background: #ffffff; }}
    .stat strong {{ display: block; color: var(--accent); font-size: 20px; }}
    pre {{ background: #ffffff; border: 1px solid var(--line); padding: 16px; overflow: auto; }}
    table {{ width: 100%; border-collapse: collapse; background: #ffffff; }}
    th, td {{ border: 1px solid var(--line); padding: 8px; vertical-align: top; text-align: left; }}
    th {{ background: #f5f5f4; position: sticky; top: 0; }}
    .timeline-wrap {{
      background: #ffffff;
      border: 1px solid var(--line);
      padding: 12px;
      overflow-x: auto;
    }}
    .timeline-svg {{
      display: block;
      min-width: 1680px;
      width: 100%;
      height: auto;
    }}
    .timeline-label {{
      fill: var(--muted);
      font-size: 12px;
    }}
    .timeline-tick {{
      fill: var(--muted);
      font-size: 11px;
    }}
    .timeline-axis {{
      stroke: #e7e5e4;
      stroke-width: 1;
    }}
    .timeline-grid {{
      stroke: #f1f5f9;
      stroke-width: 1;
    }}
    .timeline-lane {{
      stroke: #fafaf9;
      stroke-width: 1;
    }}
    .timeline-cluster:hover {{
      stroke: #111827;
      stroke-width: 2;
    }}
  </style>
</head>
<body>
  <h1>Function Clusters V2</h1>
  <p>Recursive community clustering over sparse function similarity edges.</p>
  <section class="stats">
    <div class="stat"><strong>{len(function_map)}</strong>functions</div>
    <div class="stat"><strong>{len(edges)}</strong>edges</div>
    <div class="stat"><strong>{len(flatten_leaves(root))}</strong>leaf clusters</div>
    <div class="stat"><strong>{len(spans)}</strong>rename spans</div>
  </section>
  <section>
    <h2>Cluster Timeline</h2>
    <p>Rows are tree depths. Horizontal position is the function-address span, scaled across the full binary range. Hover a block for cluster details.</p>
    <div class="timeline-wrap">
      {timeline_svg}
    </div>
  </section>
  <section>
    <h2>Cluster Tree</h2>
    <pre>{tree_text}</pre>
  </section>
  <section>
    <h2>Renameable Spans</h2>
    <table>
      <thead>
        <tr>
          <th>Leaf</th>
          <th>Size</th>
          <th>Span</th>
          <th>Unnamed Ratio</th>
          <th>Prefixes</th>
          <th>Top Globals</th>
          <th>Sample Functions</th>
        </tr>
      </thead>
      <tbody>
        {''.join(span_rows)}
      </tbody>
    </table>
  </section>
</body>
</html>
"""
	(output_dir / "cluster_report.html").write_text(document, encoding="utf-8")


def main(argv: list[str]) -> int:
	if len(argv) not in (2, 3):
		print(
			"usage: tools/function_cluster_v2.py FULL_DECOMPILE_DIR [OUTPUT_DIR]",
			file=sys.stderr,
		)
		return 1

	full_decompile_dir = Path(argv[1]).resolve()
	output_dir = Path(argv[2]).resolve() if len(argv) == 3 else DEFAULT_OUTPUT_DIR.resolve()
	client = GhidraClient(DEFAULT_GHIDRA_BASE)

	print(f"Loading full-decompile folder from {full_decompile_dir}...", file=sys.stderr)
	file_map = parse_full_decompile_folder(full_decompile_dir)

	print("Fetching function metadata from Ghidra...", file=sys.stderr)
	functions = fetch_functions(client)
	functions = [func for func in functions if func.addr in file_map]
	if not functions:
		print("No Ghidra functions matched the full-decompile folder.", file=sys.stderr)
		return 1

	function_map.clear()
	function_map.update({func.addr: func for func in functions})

	print(f"Fetching call graph for {len(functions)} functions...", file=sys.stderr)
	fetch_call_graph(client, functions)

	print("Loading globals and strings from decompile files...", file=sys.stderr)
	load_decompile_features(file_map, functions)

	print("Building sparse candidates...", file=sys.stderr)
	candidates, global_postings, string_postings, call_feature_postings = build_candidates(functions)

	print("Scoring similarity edges...", file=sys.stderr)
	edges = score_edges(functions, candidates, global_postings, string_postings, call_feature_postings)

	print("Running recursive community clustering...", file=sys.stderr)
	graph = build_similarity_graph(functions, edges)
	root = recursive_partition(graph, [func.addr for func in functions], 0, [0])

	print("Mining rename-oriented address spans...", file=sys.stderr)
	membership = build_leaf_membership(root)
	spans = mine_address_spans(functions, membership)

	print(f"Writing reports to {output_dir}...", file=sys.stderr)
	write_outputs(output_dir, root, edges, spans)

	print(
		f"Done. functions={len(functions)} edges={len(edges)} leaves={len(flatten_leaves(root))} spans={len(spans)}",
		file=sys.stderr,
	)
	return 0


if __name__ == "__main__":
	raise SystemExit(main(sys.argv))
