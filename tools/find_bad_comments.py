#!/usr/bin/env python3
"""Rank source comment lines that may be vague, redundant, or stale.

Dependencies: pip install mlx tokenizers huggingface_hub

The model is cached by huggingface_hub. Embeddings use the system temporary
directory by default so repeated runs can reuse them.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


MODEL_ID = "jinaai/jina-code-embeddings-0.5b-mlx"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
DEFAULT_QUERIES = (
    "A journal-style code comment saying that earlier or previous code was wrong, changed, fixed, moved, added, or replaced.",
    "A temporary development note mentioning a phase, future work, a future refactor, or what will happen once another system is reconstructed.",
    "A stale code comment claiming behavior is stubbed, missing, provisional, temporary, or not reconstructed even though the nearby implementation may supersede it.",
    "A comment describing the current port, clean-room implementation, or build instead of documenting the behavior and its reason.",
    "A vague comment fragment that only says TODO, stubbed, unusable, deferred, or not reconstructed without identifying the missing behavior.",
)


@dataclass(frozen=True)
class Comment:
    path: Path
    line: int
    text: str


def comment_lines(path: Path) -> list[Comment]:
    """Extract // and /* */ comment text, preserving its source line."""
    comments: list[Comment] = []
    in_block = False
    for line_number, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
        cursor = 0
        while cursor < len(raw):
            if in_block:
                end = raw.find("*/", cursor)
                text = raw[cursor : end if end >= 0 else len(raw)]
                cursor = end + 2 if end >= 0 else len(raw)
                in_block = end < 0
            else:
                slash = raw.find("//", cursor)
                block = raw.find("/*", cursor)
                starts = [pos for pos in (slash, block) if pos >= 0]
                if not starts:
                    break
                start = min(starts)
                if start == slash:
                    text = raw[start + 2 :]
                    cursor = len(raw)
                else:
                    end = raw.find("*/", start + 2)
                    text = raw[start + 2 : end if end >= 0 else len(raw)]
                    cursor = end + 2 if end >= 0 else len(raw)
                    in_block = end < 0

            text = re.sub(r"^\s*\*?\s?", "", text).strip()
            if len(text) >= 8:
                comments.append(Comment(path, line_number, text))
    return comments


def source_files(roots: list[Path]) -> list[Path]:
    files: set[Path] = set()
    for root in roots:
        if root.is_file() and root.suffix.lower() in SOURCE_SUFFIXES:
            files.add(root)
        elif root.is_dir():
            files.update(
                path
                for path in root.rglob("*")
                if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
            )
    return sorted(files)


def load_model(model_dir: str | None):
    try:
        import mlx.core as mx
        from huggingface_hub import snapshot_download
        from tokenizers import Tokenizer
        from tqdm import tqdm
    except ImportError as error:
        raise SystemExit(
            "Missing dependency. Run: pip install mlx tokenizers huggingface_hub"
        ) from error

    directory = Path(model_dir or snapshot_download(MODEL_ID))
    spec = importlib.util.spec_from_file_location("jina_code_embeddings_mlx", directory / "model.py")
    if spec is None or spec.loader is None:
        raise SystemExit(f"Could not load {directory / 'model.py'}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)

    config = json.loads((directory / "config.json").read_text())
    model = module.JinaCodeEmbeddingModel(config)
    model.load_weights(list(mx.load(str(directory / "model.safetensors")).items()))
    mx.eval(model.parameters())
    tokenizer = Tokenizer.from_file(str(directory / "tokenizer.json"))
    return mx, model, tokenizer, tqdm


def rank_comments(
    comments: list[Comment],
    queries: list[str],
    model_dir: str | None,
    batch_size: int,
    cache_dir: Path | None,
) -> list[tuple[float, int, Comment]]:
    mx, model, tokenizer, tqdm = load_model(model_dir)
    query_vectors = model.encode(
        queries, tokenizer, task="qa", prompt_type="query", truncate_dim=256
    )
    mx.eval(query_vectors)

    fingerprint = hashlib.sha256()
    fingerprint.update(f"v1\0{model_dir or MODEL_ID}\0qa\0{256}\0".encode())
    for item in comments:
        fingerprint.update(f"{item.path}\0{item.line}\0{item.text}\0".encode())
    cache_path = cache_dir / f"{fingerprint.hexdigest()}.npz" if cache_dir else None

    if cache_path and cache_path.exists():
        print(f"Loading cached embeddings from {cache_path}", file=sys.stderr)
        vectors = mx.load(str(cache_path))["embeddings"]
    else:
        if cache_dir and cache_dir.exists():
            for old_cache in cache_dir.glob("*.npz"):
                old_cache.unlink()
        batches = []
        offsets = range(0, len(comments), batch_size)
        for offset in tqdm(
            offsets,
            total=math.ceil(len(comments) / batch_size),
            desc="Embedding comments",
            unit="batch",
        ):
            batch = comments[offset : offset + batch_size]
            batch_vectors = model.encode(
                [item.text for item in batch],
                tokenizer,
                task="qa",
                prompt_type="passage",
                truncate_dim=256,
            )
            mx.eval(batch_vectors)
            batches.append(batch_vectors)
        vectors = mx.concatenate(batches)
        mx.eval(vectors)
        if cache_path:
            cache_path.parent.mkdir(parents=True, exist_ok=True)
            mx.savez(str(cache_path), embeddings=vectors)
            print(f"Cached embeddings in {cache_path}", file=sys.stderr)

    scores = mx.matmul(vectors, query_vectors.T)
    mx.eval(scores)
    ranked: list[tuple[float, int, Comment]] = []
    for item, row in zip(comments, scores.tolist(), strict=True):
        best_query = max(range(len(row)), key=row.__getitem__)
        ranked.append((row[best_query], best_query, item))
    return sorted(ranked, key=lambda result: result[0], reverse=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, default=[Path("src")])
    parser.add_argument("-n", "--limit", type=int, default=50)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--model-dir", help="Use an already-downloaded model directory")
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=Path(tempfile.gettempdir()) / "evnova-bad-comments",
        help="Embedding cache directory (default: %(default)s)",
    )
    parser.add_argument("--no-cache", action="store_true")
    parser.add_argument(
        "-q", "--query", action="append", help="Bad-comment description; repeatable"
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    files = source_files(args.paths)
    comments = [comment for path in files for comment in comment_lines(path)]
    if not comments:
        print("No comment lines found.", file=sys.stderr)
        return 1

    queries = args.query or list(DEFAULT_QUERIES)
    print(f"Embedding {len(comments)} comment lines from {len(files)} files...", file=sys.stderr)
    cache_dir = None if args.no_cache else args.cache_dir
    ranked = rank_comments(comments, queries, args.model_dir, args.batch_size, cache_dir)
    for score, query_index, comment in ranked[: args.limit]:
        print(f"{score:.4f}\t{comment.path}:{comment.line}\tq{query_index + 1}\t{comment.text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
