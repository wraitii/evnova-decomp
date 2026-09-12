#!/usr/bin/env python3
"""Rank source comment lines and Markdown prose that may be vague, redundant, or stale.

C/C++ sources are scanned for // and /* */ comments; consecutive standalone //
lines are merged into one logical block. Markdown files are scanned
paragraph-by-paragraph (fenced code blocks are skipped).

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
MARKDOWN_SUFFIXES = {".md", ".markdown"}
TEXT_SUFFIXES = SOURCE_SUFFIXES | MARKDOWN_SUFFIXES
DEFAULT_QUERIES = (
    "Previously this was wrong; changed it to the correct behavior.",
    "Phase 2 will wire this up once the system is reconstructed.",
    "Stubbed for now; not reconstructed.",
    "Clean-room port of the original function; keep in sync.",
    "TODO: stub.",
    "Decoded from the raw bytes on 2025-08-09; last updated later.",
)


@dataclass(frozen=True)
class Comment:
    path: Path
    line: int
    text: str


def markdown_blocks(path: Path) -> list[Comment]:
    """Extract prose paragraphs and headings, skipping fenced code blocks."""
    blocks: list[Comment] = []
    paragraph: list[str] = []
    start_line = 0
    in_fence = False
    fence = ""

    def flush() -> None:
        nonlocal paragraph, start_line
        text = re.sub(r"\s+", " ", " ".join(part.strip() for part in paragraph)).strip()
        if len(text) >= 8:
            blocks.append(Comment(path, start_line, text))
        paragraph = []

    for line_number, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
        stripped = raw.strip()
        if stripped.startswith(("```", "~~~")):
            if not in_fence:
                in_fence, fence = True, stripped[:3]
                flush()
            elif stripped.startswith(fence):
                in_fence, fence = False, ""
            continue
        if in_fence:
            continue
        if not stripped or re.fullmatch(r"[-*_=]{3,}", stripped) or re.fullmatch(r"\|?[\s:|-]+\|?", stripped):
            flush()
            continue
        heading = re.match(r"^#{1,6}\s+(.*)$", stripped)
        if heading:
            flush()
            text = heading.group(1).strip()
            if len(text) >= 8:
                blocks.append(Comment(path, line_number, text))
            continue
        content = re.sub(r"^(?:>\s*|[-*+]\s+|\d+[.)]\s+)+", "", stripped)
        if not paragraph:
            start_line = line_number
        paragraph.append(content)
    flush()
    return blocks


def comment_lines(path: Path) -> list[Comment]:
    """Extract // and /* */ comment text, or Markdown prose, preserving source lines."""
    if path.suffix.lower() in MARKDOWN_SUFFIXES:
        return markdown_blocks(path)

    # (line, text, mergeable) where mergeable marks a standalone // line that can
    # join the previous one into a single logical comment block.
    segments: list[tuple[int, str, bool]] = []
    in_block = False
    for line_number, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
        cursor = 0
        while cursor < len(raw):
            if in_block:
                end = raw.find("*/", cursor)
                text = raw[cursor : end if end >= 0 else len(raw)]
                cursor = end + 2 if end >= 0 else len(raw)
                in_block = end < 0
                mergeable = False
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
                    mergeable = raw[:start].strip() == ""
                else:
                    end = raw.find("*/", start + 2)
                    text = raw[start + 2 : end if end >= 0 else len(raw)]
                    cursor = end + 2 if end >= 0 else len(raw)
                    in_block = end < 0
                    mergeable = False

            text = re.sub(r"^\s*\*?\s?", "", text).strip()
            if len(text) >= 8:
                segments.append((line_number, text, mergeable))

    comments: list[Comment] = []
    last_line = -2
    previous_mergeable = False
    for line_number, text, mergeable in segments:
        if comments and mergeable and previous_mergeable and line_number == last_line + 1:
            previous = comments[-1]
            comments[-1] = Comment(previous.path, previous.line, f"{previous.text} {text}")
        else:
            comments.append(Comment(path, line_number, text))
        last_line = line_number
        previous_mergeable = mergeable
    return comments


def source_files(roots: list[Path]) -> list[Path]:
    files: set[Path] = set()
    for root in roots:
        if root.is_file() and root.suffix.lower() in TEXT_SUFFIXES:
            files.add(root)
        elif root.is_dir():
            files.update(
                path
                for path in root.rglob("*")
                if path.is_file()
                and path.suffix.lower() in TEXT_SUFFIXES
                and not _is_generated(path.relative_to(root))
            )
    return sorted(files)


def _is_generated(relative: Path) -> bool:
    """Skip vendored/generated trees such as docs/assets (regenerated artwork)."""
    parts = relative.parts
    return bool(parts) and parts[0] in {"assets"}


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
    parser.add_argument("paths", nargs="*", type=Path, default=[Path("src"), Path("docs")])
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
    print(f"Embedding {len(comments)} comment/prose units from {len(files)} files...", file=sys.stderr)
    cache_dir = None if args.no_cache else args.cache_dir
    ranked = rank_comments(comments, queries, args.model_dir, args.batch_size, cache_dir)
    for score, query_index, comment in ranked[: args.limit]:
        print(f"{score:.4f}\t{comment.path}:{comment.line}\tq{query_index + 1}\t{comment.text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
