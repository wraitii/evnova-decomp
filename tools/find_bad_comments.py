#!/usr/bin/env python3
"""Rank source comment lines and Markdown prose that may be vague, redundant, or stale.

C/C++ sources are scanned for // and /* */ comments; consecutive standalone //
lines are merged into one logical block. Markdown files are scanned
paragraph-by-paragraph (fenced code blocks are skipped).

Dependencies: pip install mlx tokenizers huggingface_hub (see semantic_embed).

The model is cached by huggingface_hub. Embeddings use the system temporary
directory by default so repeated runs can reuse them.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import semantic_embed


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


def rank_comments(
    comments: list[Comment],
    queries: list[str],
    model_dir: str | None,
    batch_size: int,
    cache_dir: Path | None,
) -> list[tuple[float, int, Comment]]:
    embedder = semantic_embed.Embedder(model_dir, batch_size=batch_size)
    query_vectors = embedder.embed(
        queries, task="qa", prompt_type=semantic_embed.PROMPT_QUERY, progress=False
    )
    passage_vectors = embedder.embed(
        [item.text for item in comments],
        task="qa",
        prompt_type=semantic_embed.PROMPT_PASSAGE,
        cache_dir=cache_dir,
        namespace="bad-comments-",
        prune=True,
    )
    scores = semantic_embed.dot(passage_vectors, query_vectors)
    ranked = [
        (score, query_index, comment)
        for (score, query_index), comment in zip(
            semantic_embed.rank_by_best_query(scores.tolist()), comments, strict=True
        )
    ]
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
    semantic_embed.write_lines(
        [
            f"{score:.4f}\t{comment.path}:{comment.line}\tq{query_index + 1}\t{comment.text}"
            for score, query_index, comment in ranked[: args.limit]
        ]
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
