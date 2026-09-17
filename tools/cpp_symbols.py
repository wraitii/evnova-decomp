#!/usr/bin/env python3
"""Extract C++ function definitions from the recomp sources via clangd.

clangd is driven over LSP (``textDocument/documentSymbol``) using the project's
``compile_commands.json``, so extraction matches the real build instead of a
hand-rolled brace matcher. Returns one :class:`CppFunction` per definition that
has a body (declarations are skipped).

Requires ``clangd`` on PATH (or a path passed explicitly) and a compile
database; generate one with ``cmake --preset release``.
"""

from __future__ import annotations

import json
import queue
import shutil
import subprocess
import sys
import threading
from dataclasses import dataclass
from pathlib import Path

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
GENERATED_DIRS = {"assets", "build"}
# Max documents opened in one pipelined LSP batch (bounds memory in clangd).
PIPELINE_WINDOW = 64

# LSP SymbolKind values.
_FUNCTION_KINDS = {6, 9, 12}  # Method, Constructor, Function
_CONTAINER_KINDS = {2, 3, 4, 5, 11, 23}  # Module, Namespace, Package, Class, Interface, Struct


@dataclass(frozen=True)
class CppFunction:
    path: Path
    name: str
    line: int
    text: str

    @property
    def label(self) -> str:
        shown = self.path.name
        return f"{shown}:{self.line} {self.name}"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def default_compile_commands_dir(explicit: Path | None = None) -> Path:
    if explicit is not None:
        return explicit
    root = repo_root()
    for candidate in (root / "build" / "release", root / "build" / "debug"):
        if (candidate / "compile_commands.json").is_file():
            return candidate
    raise FileNotFoundError(
        "No compile_commands.json found under build/release or build/debug; "
        "configure with cmake --preset release"
    )


def source_files(roots: list[Path]) -> list[Path]:
    """Discover C++ sources and headers under ``roots``, skipping generated trees."""
    files: set[Path] = set()
    for root in roots:
        root = root if root.is_absolute() else repo_root() / root
        if root.is_file() and root.suffix.lower() in SOURCE_SUFFIXES:
            files.add(root)
        elif root.is_dir():
            for path in root.rglob("*"):
                if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
                    continue
                rel = path.relative_to(root)
                if any(part in GENERATED_DIRS for part in rel.parts):
                    continue
                files.add(path)
    return sorted(files)


def _find_clangd() -> str:
    return shutil.which("clangd") or "/usr/bin/clangd"


class ClangdSymbols:
    """Minimal LSP client exposing clangd document symbols.

    A background thread drains stdout into a queue, so pipelined batches cannot
    deadlock when clangd's response pipe fills while requests are still being
    written.
    """

    def __init__(self, compile_commands_dir: Path, root: Path | None = None) -> None:
        self.root = root or repo_root()
        self._next_id = 0
        self._messages: queue.Queue[dict | None] = queue.Queue()
        self._process = subprocess.Popen(
            [
                _find_clangd(),
                f"--compile-commands-dir={compile_commands_dir}",
                "--background-index=false",
                "--pch-storage=disk",
                "--log=error",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        self._reader = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader.start()
        self._initialize()

    def __enter__(self) -> "ClangdSymbols":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def _send(self, payload: dict) -> None:
        assert self._process.stdin is not None
        body = json.dumps(payload).encode()
        self._process.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self._process.stdin.flush()

    def _reader_loop(self) -> None:
        while True:
            message = self._read_raw()
            self._messages.put(message)
            if message is None:
                return

    def _read_raw(self) -> dict | None:
        assert self._process.stdout is not None
        header = b""
        while b"\r\n\r\n" not in header:
            char = self._process.stdout.read(1)
            if not char:
                return None
            header += char
        length = 0
        for line in header.decode().split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1])
        body = self._process.stdout.read(length)
        return json.loads(body)

    def _next_message(self, method: str) -> dict:
        message = self._messages.get()
        if message is None:
            raise RuntimeError(f"clangd closed while waiting for {method}")
        return message

    def _request(self, method: str, params: dict) -> dict:
        self._next_id += 1
        request_id = self._next_id
        self._send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        while True:
            message = self._next_message(method)
            if "method" in message and "id" in message:
                # Server-initiated request (configuration/progress): answer benignly.
                self._send({"jsonrpc": "2.0", "id": message["id"], "result": None})
                continue
            if message.get("id") == request_id:
                if "error" in message:
                    raise RuntimeError(f"clangd error for {method}: {message['error']}")
                return message.get("result") or {}

    def _initialize(self) -> None:
        self._request(
            "initialize",
            {
                "processId": None,
                "rootUri": self.root.as_uri(),
                "capabilities": {
                    "textDocument": {
                        "documentSymbol": {"hierarchicalDocumentSymbolSupport": True}
                    }
                },
            },
        )
        self._send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

    def document_symbols(self, path: Path) -> list[dict]:
        resolved = path.resolve()
        return self.document_symbols_batch([resolved]).get(resolved, [])

    def document_symbols_batch(self, paths: list[Path]) -> dict[Path, list[dict]]:
        """Return symbols per file, pipelining all opens and requests at once."""
        resolved = [path.resolve() for path in paths]
        for path in resolved:
            self._send(
                {
                    "jsonrpc": "2.0",
                    "method": "textDocument/didOpen",
                    "params": {
                        "textDocument": {
                            "uri": path.as_uri(),
                            "languageId": "cpp",
                            "version": 1,
                            "text": path.read_text(errors="replace"),
                        }
                    },
                }
            )

        pending: dict[int, Path] = {}
        for path in resolved:
            self._next_id += 1
            pending[self._next_id] = path
            self._send(
                {
                    "jsonrpc": "2.0",
                    "id": self._next_id,
                    "method": "textDocument/documentSymbol",
                    "params": {"textDocument": {"uri": path.as_uri()}},
                }
            )

        results: dict[Path, list[dict]] = {}
        while pending:
            message = self._next_message("textDocument/documentSymbol")
            if "method" in message and "id" in message:
                self._send({"jsonrpc": "2.0", "id": message["id"], "result": None})
                continue
            request_id = message.get("id")
            if request_id not in pending:
                continue
            path = pending.pop(request_id)
            if "error" in message:
                print(f"clangd error for {path}: {message['error']}", file=sys.stderr)
                results[path] = []
            else:
                results[path] = message.get("result") or []
        return results

    def close(self) -> None:
        try:
            self._request("shutdown", {})
            self._send({"jsonrpc": "2.0", "method": "exit", "params": {}})
        except Exception:  # noqa: BLE001 - best-effort teardown
            pass
        finally:
            if self._process.poll() is None:
                self._process.terminate()
            self._reader.join(timeout=2)


def _collect(
    symbols: list[dict],
    path: Path,
    lines: list[str],
    prefix: str,
    out: list[CppFunction],
) -> None:
    for symbol in symbols:
        name = str(symbol.get("name", ""))
        kind = symbol.get("kind")
        full_name = f"{prefix}{name}"
        range_ = symbol.get("range")
        if kind in _FUNCTION_KINDS and range_ is not None:
            start = range_["start"]["line"]
            end = range_["end"]["line"]
            text = "\n".join(lines[start : end + 1])
            if "{" in text:  # skip pure declarations
                out.append(CppFunction(path, full_name, start + 1, text))
        if kind in _CONTAINER_KINDS:
            _collect(symbol.get("children") or [], path, lines, f"{full_name}::", out)


def extract_functions(
    files: list[Path],
    compile_commands_dir: Path | None = None,
    *,
    progress: bool = True,
) -> list[CppFunction]:
    """Return every function/method definition found across ``files``.

    A single clangd instance parses files in pipelined batches: all documents in
    a window are opened and requested up front, letting clangd schedule the
    parses across its own thread pool.
    """
    if not files:
        return []
    directory = default_compile_commands_dir(compile_commands_dir)
    bar = _progress_bar(len(files), "Extracting symbols") if progress else None
    try:
        clangd = ClangdSymbols(directory)
    except Exception as error:  # noqa: BLE001 - report and return nothing
        print(f"clangd unavailable: {error}", file=sys.stderr)
        if bar is not None:
            bar.close()
        return []

    extracted: dict[Path, list[CppFunction]] = {}
    try:
        for start in range(0, len(files), PIPELINE_WINDOW):
            group = files[start : start + PIPELINE_WINDOW]
            try:
                symbols_by_path = clangd.document_symbols_batch(group)
            except RuntimeError as error:
                print(f"clangd batch failed: {error}", file=sys.stderr)
                if bar is not None:
                    bar.update(len(group))
                continue
            for path in group:
                symbols = symbols_by_path.get(path.resolve())
                if symbols is None:
                    continue
                lines = path.read_text(errors="replace").splitlines()
                functions: list[CppFunction] = []
                _collect(symbols, path, lines, "", functions)
                extracted[path] = functions
            if bar is not None:
                bar.update(len(group))
    finally:
        clangd.close()
        if bar is not None:
            bar.close()
    return sorted(
        (function for funcs in extracted.values() for function in funcs),
        key=lambda function: (str(function.path), function.line),
    )


def _progress_bar(total: int, description: str) -> object:
    try:
        from tqdm import tqdm
    except ImportError:
        return None
    return tqdm(total=total, desc=description, unit="file")
