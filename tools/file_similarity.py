#!/usr/bin/env python3
"""Rank source files for merging/splitting from function code embeddings.

Reuses the clangd extraction (``cpp_symbols``) and Jina ``code2code`` embeddings
(``semantic_embed``) from ``find_duplicate_functions``, then aggregates the
per-function vectors into per-file vectors:

  * cross-file cosine similarity surfaces files that could be merged;
  * mean intra-file function similarity (cohesion) surfaces files that could be
    split, plus where each file's functions find their nearest neighbour;
  * within-file function pairs surface copy-paste / near-duplicate helpers.

Dependencies match ``find_duplicate_functions``: clangd, a compile database, and
the tools/.venv environment.
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path
from typing import Sequence

import numpy as np

import cpp_symbols
import find_duplicate_functions
import semantic_embed

MODE_CHOICES = ("all", "files", "cohesion", "intra")
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx"}


def is_header_sibling(left: Path, right: Path) -> bool:
    """True for a source/header pair such as ``foo.cpp`` and ``foo.hpp``."""
    return (
        left.parent == right.parent
        and left.stem == right.stem
        and (left.suffix in HEADER_SUFFIXES) != (right.suffix in HEADER_SUFFIXES)
    )


def l2_normalize(matrix: np.ndarray) -> np.ndarray:
    """Row-wise L2 normalization; zero rows are left untouched."""
    matrix = np.asarray(matrix, dtype=np.float64)
    if matrix.ndim == 1:
        norm = float(np.linalg.norm(matrix))
        return matrix if norm == 0.0 else matrix / norm
    norms = np.linalg.norm(matrix, axis=1, keepdims=True)
    return matrix / np.where(norms == 0.0, 1.0, norms)


def group_by_file(paths: Sequence[Path]) -> dict[Path, list[int]]:
    groups: dict[Path, list[int]] = {}
    for index, path in enumerate(paths):
        groups.setdefault(path, []).append(index)
    return groups


def file_vectors(
    vectors: np.ndarray, groups: dict[Path, list[int]]
) -> tuple[list[Path], np.ndarray]:
    """Mean-pool each file's function vectors, then normalize the result."""
    files = sorted(groups)
    pooled = [l2_normalize(vectors[groups[path]].mean(axis=0)) for path in files]
    if not pooled:
        return files, np.zeros((0, vectors.shape[1]))
    return files, np.stack(pooled)


def cohesion_scores(
    vectors: np.ndarray, groups: dict[Path, list[int]]
) -> dict[Path, float]:
    """Mean pairwise cosine similarity of each file's own functions."""
    scores: dict[Path, float] = {}
    for path, indices in groups.items():
        count = len(indices)
        if count < 2:
            continue
        block = l2_normalize(vectors[indices])
        similarity = block @ block.T
        scores[path] = float((similarity.sum() - count) / (count * (count - 1)))
    return scores


def best_external_match(
    vectors: np.ndarray, paths: Sequence[Path], skip_siblings: bool = True
) -> list[tuple[int, int, float]]:
    """For each function, its most similar function in a different file.

    Functions in a header/source sibling file are ignored when
    ``skip_siblings`` is set; an unmatched function gets neighbour ``-1``.
    """
    normalized = l2_normalize(vectors)
    similarity = normalized @ normalized.T
    matches: list[tuple[int, int, float]] = []
    for index, path in enumerate(paths):
        row = similarity[index].copy()
        excluded = [
            other
            for other, other_path in enumerate(paths)
            if other_path == path
            or (skip_siblings and is_header_sibling(path, other_path))
        ]
        row[excluded] = -np.inf
        best = float(row.max())
        if not np.isfinite(best):
            matches.append((index, -1, float("-inf")))
            continue
        matches.append((index, int(np.argmax(row)), best))
    return matches


def file_similarity_pairs(
    files: Sequence[Path],
    pooled: np.ndarray,
    threshold: float,
    skip_siblings: bool = True,
) -> list[tuple[float, Path, Path]]:
    similarity = pooled @ pooled.T
    pairs = []
    for left in range(len(files)):
        for right in range(left + 1, len(files)):
            if skip_siblings and is_header_sibling(files[left], files[right]):
                continue
            score = float(similarity[left, right])
            if score >= threshold:
                pairs.append((score, files[left], files[right]))
    return sorted(pairs, reverse=True)


def intra_file_pairs(
    vectors: np.ndarray, groups: dict[Path, list[int]], top_k: int, threshold: float
) -> list[tuple[float, int, int]]:
    pairs = []
    for indices in groups.values():
        if len(indices) < 2:
            continue
        block = l2_normalize(vectors[indices])
        similarity = block @ block.T
        for left in range(len(indices)):
            for right in range(left + 1, len(indices)):
                score = float(similarity[left, right])
                if score >= threshold:
                    pairs.append((score, indices[left], indices[right]))
    return sorted(pairs, reverse=True)[:top_k]


def dominant_external_file(
    indices: Sequence[int],
    matches: Sequence[tuple[int, int, float]],
    paths: Sequence[Path],
) -> tuple[Path | None, int]:
    """Most common external file that a file's functions match, and its count."""
    destinations = Counter(
        paths[matches[index][1]] for index in indices if matches[index][1] >= 0
    )
    if not destinations:
        return None, 0
    return destinations.most_common(1)[0]


def line_count(path: Path) -> int:
    """Total number of lines in a source file."""
    try:
        return len(path.read_text(errors="replace").splitlines())
    except OSError:
        return 0


def _relative(path: Path, root: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        type=Path,
        default=[Path("src")],
        help="Source roots to scan (default: %(default)s)",
    )
    parser.add_argument(
        "--mode",
        choices=MODE_CHOICES,
        default="all",
        help="Which report sections to print (default: %(default)s)",
    )
    parser.add_argument("-n", "--limit", type=int, default=40, help="Rows per section")
    parser.add_argument(
        "--threshold", type=float, default=0.0, help="Minimum file-pair similarity"
    )
    parser.add_argument(
        "--intra-threshold",
        type=float,
        default=0.75,
        help="Minimum within-file function similarity",
    )
    parser.add_argument("--top-k", type=int, default=20, help="Within-file pairs to print")
    parser.add_argument(
        "--include-header-siblings",
        action="store_true",
        help="Also compare a source file with its own header (default: skip)",
    )
    parser.add_argument("--min-chars", type=int, default=80, help="Skip shorter functions")
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument(
        "--token-budget",
        type=int,
        default=semantic_embed.DEFAULT_TOKEN_BUDGET,
        help="Max padded tokens per forward pass; long functions use smaller batches",
    )
    parser.add_argument(
        "--max-length",
        type=int,
        help="Max tokens per function (default: model limit, 8192)",
    )
    parser.add_argument(
        "--compile-commands-dir",
        type=Path,
        help="Directory holding compile_commands.json (default: build/release)",
    )
    parser.add_argument("--model-dir", help="Use an already-downloaded model directory")
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=Path("/tmp") / "evnova-file-similarity",
        help="Embedding cache directory (default: %(default)s)",
    )
    parser.add_argument("--no-cache", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        units, skipped = find_duplicate_functions.collect_units(
            args.paths, args.compile_commands_dir, args.min_chars
        )
    except FileNotFoundError as error:
        print(error, file=sys.stderr)
        return 1
    if len(units) < 2:
        print(f"Need at least two functions (found {len(units)}).", file=sys.stderr)
        return 1
    print(
        f"Embedding {len(units)} functions from "
        f"{len({unit.path for unit in units})} files ({skipped} skipped)...",
        file=sys.stderr,
    )

    embedder = semantic_embed.Embedder(args.model_dir, batch_size=args.batch_size)
    cache_dir = None if args.no_cache else args.cache_dir
    vectors = np.asarray(
        embedder.embed(
            [unit.text for unit in units],
            task="code2code",
            prompt_type=semantic_embed.PROMPT_PASSAGE,
            max_length=args.max_length,
            token_budget=args.token_budget,
            cache_dir=cache_dir,
            namespace="file-sim-",
            prune=True,
        )
    )

    paths = [unit.path for unit in units]
    groups = group_by_file(paths)
    files, pooled = file_vectors(vectors, groups)
    skip_siblings = not args.include_header_siblings
    matches = best_external_match(vectors, paths, skip_siblings)
    root = Path(__file__).resolve().parent.parent
    multi_file = len(files) > 1

    lines: list[str] = []
    if args.mode in ("all", "files"):
        lines.append("# Similar file pairs (merge candidates)")
        lines.append("score\tfiles\tfunctions")
        for score, left, right in file_similarity_pairs(
            files, pooled, args.threshold, skip_siblings
        )[: args.limit]:
            lines.append(
                f"{score:.4f}\t{_relative(left, root)} ({line_count(left)}l) <-> "
                f"{_relative(right, root)} ({line_count(right)}l)"
                f"\t{len(groups[left])} / {len(groups[right])}"
            )
        lines.append("")

    if args.mode in ("all", "cohesion"):
        lines.append("# File cohesion (ascending; low = split candidate)")
        lines.append("cohesion\tfunctions\tfile\tdominant external match")
        cohesion = cohesion_scores(vectors, groups)
        for path, score in sorted(cohesion.items(), key=lambda item: item[1])[
            : args.limit
        ]:
            indices = groups[path]
            if multi_file:
                destination, count = dominant_external_file(indices, matches, paths)
                dominant = (
                    f"{_relative(destination, root)} ({count}/{len(indices)})"
                    if destination is not None
                    else "-"
                )
            else:
                dominant = "-"
            lines.append(
                f"{score:.4f}\t{len(indices)}\t{_relative(path, root)}"
                f" ({line_count(path)}l)\t{dominant}"
            )
        lines.append("")

    if args.mode in ("all", "intra"):
        lines.append("# Similar function pairs within a file (near-duplicates)")
        lines.append("score\tfunctions")
        for score, left, right in intra_file_pairs(
            vectors, groups, args.top_k, args.intra_threshold
        )[: args.limit]:
            lines.append(f"{score:.4f}\t{units[left].label}\t{units[right].label}")
        lines.append("")

    semantic_embed.write_lines(lines)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
