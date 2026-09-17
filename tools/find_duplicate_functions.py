#!/usr/bin/env python3
"""Find near-duplicate functions in the recomp sources via code embeddings.

Function definitions are extracted from the port's own C++ sources with clangd
(see cpp_symbols.py), embedded with the ``code2code`` task, and compared by
cosine similarity. Output is the most similar pairs above ``--threshold``,
which helps spot redundant helpers, copy-paste ports, and near-identical
wrappers.

Dependencies: clangd, a CMake compile database (``cmake --preset release``), and
the tools/.venv environment (mlx, tokenizers, huggingface_hub, numpy).
"""

from __future__ import annotations

import argparse
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import cpp_symbols
import semantic_embed


@dataclass(frozen=True)
class FunctionUnit:
    path: Path
    label: str
    text: str


def collect_units(
    roots: list[Path],
    compile_commands_dir: Path | None,
    min_chars: int,
) -> tuple[list[FunctionUnit], int]:
    """Extract function definitions; returns (units, skipped_short)."""
    files = cpp_symbols.source_files(roots)
    functions = cpp_symbols.extract_functions(files, compile_commands_dir)
    units: list[FunctionUnit] = []
    skipped = 0
    for function in functions:
        if len(function.text) < min_chars:
            skipped += 1
            continue
        units.append(FunctionUnit(function.path, function.label, function.text))
    return units, skipped


def dedupe_pairs(
    pairs: list[tuple[int, int, float]],
    units: list[FunctionUnit],
    limit: int,
) -> list[tuple[float, FunctionUnit, FunctionUnit]]:
    best: dict[tuple[int, int], float] = {}
    for left, right, score in pairs:
        key = (min(left, right), max(left, right))
        if score > best.get(key, float("-inf")):
            best[key] = score
    ranked = sorted(best.items(), key=lambda item: item[1], reverse=True)
    return [(score, units[a], units[b]) for (a, b), score in ranked[:limit]]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        type=Path,
        default=[Path("src")],
        help="Source roots to scan (default: %(default)s)",
    )
    parser.add_argument("-n", "--limit", type=int, default=100, help="Pairs to print")
    parser.add_argument(
        "-t", "--threshold", type=float, default=0.9, help="Minimum cosine similarity"
    )
    parser.add_argument("--top-k", type=int, default=5, help="Neighbours per function")
    parser.add_argument("--min-chars", type=int, default=80, help="Skip shorter units")
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
        default=Path(tempfile.gettempdir()) / "evnova-func-duplicates",
        help="Embedding cache directory (default: %(default)s)",
    )
    parser.add_argument("--no-cache", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        units, skipped = collect_units(
            args.paths, args.compile_commands_dir, args.min_chars
        )
    except FileNotFoundError as error:
        print(error, file=sys.stderr)
        return 1
    if len(units) < 2:
        print(f"Need at least two functions (found {len(units)}).", file=sys.stderr)
        return 1
    print(
        f"Embedding {len(units)} functions ({skipped} skipped as too short)...",
        file=sys.stderr,
    )

    embedder = semantic_embed.Embedder(args.model_dir, batch_size=args.batch_size)
    cache_dir = None if args.no_cache else args.cache_dir
    vectors = embedder.embed(
        [unit.text for unit in units],
        task="code2code",
        prompt_type=semantic_embed.PROMPT_PASSAGE,
        max_length=args.max_length,
        token_budget=args.token_budget,
        cache_dir=cache_dir,
        namespace="func-dupes-",
        prune=True,
    )

    pairs = list(
        semantic_embed.iter_nearest(
            vectors, top_k=args.top_k, threshold=args.threshold
        )
    )
    semantic_embed.write_lines(
        [
            f"{score:.4f}\t{left.label}\t{right.label}"
            for score, left, right in dedupe_pairs(pairs, units, args.limit)
        ]
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
