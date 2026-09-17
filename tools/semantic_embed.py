#!/usr/bin/env python3
"""Reusable MLX text-embedding backend plus similarity helpers.

Wraps the Jina Code Embeddings 0.5B MLX model with batching and an optional
on-disk vector cache, and exposes backend-level ranking helpers. Current
consumers:

  * find_bad_comments.py        - rank comment/prose units against query text
  * find_duplicate_functions.py - find near-duplicate decompiled functions
  * file_similarity.py          - merge/split hints from per-file function vectors

The heavy imports (mlx, tokenizers, huggingface_hub) are deferred until an
``Embedder`` is constructed, so the pure scoring helpers stay importable
without the model stack.

Dependencies: pip install mlx tokenizers huggingface_hub numpy
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import math
import sys
from collections.abc import Iterator, Sequence
from pathlib import Path
from typing import Any

MODEL_ID = "jinaai/jina-code-embeddings-0.5b-mlx"
EMBED_DIM = 256
DEFAULT_BATCH_SIZE = 64
# Rough cap on padded tokens per forward pass. Batches are formed from
# length-sorted texts, so long functions land in smaller batches instead of
# forcing every short function in the batch to pad up to their length.
DEFAULT_TOKEN_BUDGET = 32768

# Prompt roles accepted by the model's INSTRUCTION_CONFIG. Task names
# (nl2code, qa, code2code, code2nl, code2completion) are passed through as
# plain strings so callers can use any value the checkpoint supports.
PROMPT_QUERY = "query"
PROMPT_PASSAGE = "passage"


def _progress(iterable: Any, total: int, description: str) -> Any:
    try:
        from tqdm import tqdm
    except ImportError:
        return iterable
    return tqdm(iterable, total=total, desc=description, unit="batch")


def write_lines(lines: Sequence[str], file: Any = None) -> None:
    """Write ``lines`` atomically so tqdm never splits a result line.

    Progress bars render to stderr (including from tqdm's background monitor
    thread), while results normally go to stdout. Both directions are separate
    streams and separately buffered, so a plain ``print`` can land inside a bar
    render or flush out of order. Going through ``tqdm.write`` takes the same
    global lock, clears any live bar, and flushes the target, keeping the output
    readable. Falls back to ``print`` when tqdm is not installed.
    """
    if not lines:
        return
    payload = "\n".join(lines)
    target = file if file is not None else sys.stdout
    try:
        from tqdm import tqdm
    except ImportError:
        print(payload, file=target)
    else:
        tqdm.write(payload, file=target)
    if hasattr(target, "flush"):
        target.flush()


class Embedder:
    """Batched, cacheable encoder for the Jina Code MLX model.

    ``embed`` returns an ``(N, dim)`` matrix of L2-normalized MLX vectors
    (the model normalizes internally), ready for dot-product similarity.
    """

    def __init__(
        self,
        model_dir: str | Path | None = None,
        *,
        batch_size: int = DEFAULT_BATCH_SIZE,
        dim: int = EMBED_DIM,
    ) -> None:
        self.batch_size = batch_size
        self.dim = dim

        try:
            import mlx.core as mx
            from huggingface_hub import snapshot_download
            from tokenizers import Tokenizer
        except ImportError as error:
            raise SystemExit(
                "Missing dependency. Run: pip install mlx tokenizers huggingface_hub"
            ) from error

        self._mx = mx
        directory = Path(model_dir) if model_dir is not None else Path(snapshot_download(MODEL_ID))
        self.model_dir = directory

        spec = importlib.util.spec_from_file_location(
            "jina_code_embeddings_mlx", directory / "model.py"
        )
        if spec is None or spec.loader is None:
            raise SystemExit(f"Could not load {directory / 'model.py'}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)

        config = json.loads((directory / "config.json").read_text())
        model = module.JinaCodeEmbeddingModel(config)
        model.load_weights(list(mx.load(str(directory / "model.safetensors")).items()))
        mx.eval(model.parameters())
        self._model = model
        self._tokenizer = Tokenizer.from_file(str(directory / "tokenizer.json"))

    def embed(
        self,
        texts: Sequence[str],
        *,
        task: str = "nl2code",
        prompt_type: str = PROMPT_QUERY,
        max_length: int | None = None,
        token_budget: int = DEFAULT_TOKEN_BUDGET,
        cache_dir: Path | None = None,
        namespace: str = "",
        prune: bool = False,
        progress: bool = True,
    ) -> Any:
        """Embed ``texts`` and return an ``(N, dim)`` MLX matrix.

        Texts are bucketed by token length and batched up to ``batch_size``
        members or ``token_budget`` padded tokens, whichever binds first, so
        each function is encoded at roughly its own length and long functions
        do not inflate the cost of short ones. ``max_length`` optionally caps
        tokens per text (default: the model's 8192). Results are scattered back
        to the caller's order.

        When ``cache_dir`` is set the result is stored under a fingerprint of
        the model/task/prompt/dim and the exact text list. ``namespace`` prefixes
        cache filenames so several consumers can share a directory. ``prune``
        deletes other cached arrays in the directory before writing, which is
        safe when a single run owns the directory.
        """
        mx = self._mx
        texts = list(texts)
        cache_path: Path | None = None
        if cache_dir is not None:
            cache_dir = Path(cache_dir)
            fingerprint = hashlib.sha256()
            fingerprint.update(
                f"v3\0{self.model_dir}\0{task}\0{prompt_type}\0{self.dim}\0{max_length}\0".encode()
            )
            for text in texts:
                fingerprint.update(text.encode())
                fingerprint.update(b"\0")
            cache_path = cache_dir / f"{namespace}{fingerprint.hexdigest()}.npz"

        if cache_path is not None and cache_path.exists():
            print(f"Loading cached embeddings from {cache_path}", file=sys.stderr)
            return mx.load(str(cache_path))["embeddings"]

        if prune and cache_dir is not None and cache_dir.exists():
            for stale in cache_dir.glob("*.npz"):
                stale.unlink()

        import numpy as np

        # Token counts drive bucketing; the instruction prefix added inside the
        # model is short, so a small constant keeps the estimate conservative.
        lengths = [len(encoding.ids) + 8 for encoding in self._tokenizer.encode_batch(texts)]
        order = sorted(range(len(texts)), key=lambda index: lengths[index])
        batches: list[list[int]] = []
        current: list[int] = []
        for index in order:
            effective = lengths[index]
            if max_length is not None:
                effective = min(effective, max_length)
            if current and (
                len(current) >= self.batch_size
                or (len(current) + 1) * effective > token_budget
            ):
                batches.append(current)
                current = []
            current.append(index)
        if current:
            batches.append(current)

        offsets: object = batches
        if progress:
            offsets = _progress(batches, len(batches), "Embedding")
        results = []
        for indices in offsets:
            vectors = self._model.encode(
                [texts[index] for index in indices],
                self._tokenizer,
                task=task,
                prompt_type=prompt_type,
                max_length=max_length or 8192,
                truncate_dim=self.dim,
            )
            mx.eval(vectors)
            results.append(vectors)

        if results:
            sorted_vectors = mx.concatenate(results)
            inverse = np.empty(len(texts), dtype=np.int64)
            inverse[order] = np.arange(len(texts))
            vectors = mx.array(np.asarray(sorted_vectors)[inverse])
        else:
            vectors = mx.zeros((0, self.dim))
        mx.eval(vectors)

        if cache_path is not None:
            cache_path.parent.mkdir(parents=True, exist_ok=True)
            mx.savez(str(cache_path), embeddings=vectors)
            print(f"Cached embeddings in {cache_path}", file=sys.stderr)
        return vectors


def dot(a: Any, b: Any) -> Any:
    """Return the pairwise dot product ``a @ b.T`` for L2-normalized rows."""
    import mlx.core as mx

    return mx.matmul(a, b.T)


def rank_by_best_query(scores: Sequence[Sequence[float]]) -> list[tuple[float, int]]:
    """For each row of ``scores`` return ``(best_score, query_index)``."""
    return [(max(row), max(range(len(row)), key=row.__getitem__)) for row in scores]


def iter_nearest(
    vectors: Any,
    *,
    top_k: int = 10,
    threshold: float = 0.0,
    chunk_size: int = 512,
    progress: bool = True,
) -> Iterator[tuple[int, int, float]]:
    """Yield ``(row, neighbour, similarity)`` for each row's top-k neighbours.

    Assumes L2-normalized rows, so the dot product is cosine similarity. Rows
    are processed in chunks to bound peak memory; both directions of a pair are
    yielded, so callers that want unique pairs should key on ``(min, max)``.
    """
    import numpy as np

    count = vectors.shape[0]
    if count < 2 or top_k < 1:
        return

    offsets = range(0, count, chunk_size)
    if progress:
        offsets = _progress(offsets, math.ceil(count / chunk_size), "Similarity")
    for start in offsets:
        stop = min(start + chunk_size, count)
        block = np.asarray(dot(vectors[start:stop], vectors))
        for offset, row in enumerate(block):
            index = start + offset
            row = row.copy()
            row[index] = -np.inf
            k = min(top_k, count - 1)
            candidates = np.argpartition(row, -k)[-k:]
            for neighbour in candidates:
                score = float(row[neighbour])
                if score >= threshold:
                    yield index, int(neighbour), score
