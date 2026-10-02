#!/usr/bin/env python3
"""Generate the microexec host's port reflection from the port's own headers.

The host must set any port field the bridge names and call any ported function
with the arguments the original was called with. Rather than listing those by
hand, this reads clang's JSON AST of every `src/game` header and emits:

- a field table for every complete struct/class in namespace `game`, one entry
  per public, non-reference, non-bitfield data member. `reflect.hpp` walks and
  assigns them generically, so every port path is settable with no setter code;
- a target table: every namespace-level function declared in a header and cited
  as the port of an original address (a `// @port 0x...` marker, else the first
  `// Ghidra 0x...` citation, in the comment block right above its definition or
  declaration; only unindented comment blocks count, and a marker outranks a
  citation). `reflect.hpp` decides at compile time whether its signature can
  be driven; one it cannot is still listed, with the reason, and so is a cited
  function the host cannot take the address of (a method, a file-local helper,
  an overload), so the bridge can say why a target is not compared.

Usage (run by CMake at build time):
    gen_reflection.py --compile-commands build/release/compile_commands.json \
        --src src --out build/release/tools/microexec_evnova/host/generated
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT_NAMESPACE = "game"
HEADER_DIR = "game"  # under --src: the headers whose declarations are reflected
KEEP_FLAGS = ("-I", "-isystem", "-D", "-std=", "-arch", "-isysroot", "-iquote", "-F")
PAIRED_FLAGS = ("-isystem", "-arch", "-isysroot", "-iquote", "-I", "-D", "-F")
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "decltype", "static_assert", "alignof", "catch"}

_ADDR_RE = re.compile(r"0x([0-9a-fA-F]{1,8})\b")
_PORT_RE = re.compile(r"^\s*//\s*@port\s+(?P<addrs>0x[0-9a-fA-F]+(?:\s*,\s*0x[0-9a-fA-F]+)*)")
_GHIDRA_RE = re.compile(r"^\s*//\s*Ghidra\s+0x(?P<addr>[0-9a-fA-F]{1,8})\b")
_CALL_RE = re.compile(r"(?:^|[\s*&:])(?P<name>[A-Za-z_]\w*)\s*\($")


@dataclass
class Record:
    name: str  # fully qualified
    fields: list[str] = field(default_factory=list)


@dataclass
class Function:
    name: str  # fully qualified
    params: list[str]
    signature: str
    has_body: bool


def compiler_command(compile_commands: Path, src: Path) -> list[str]:
    """The compiler and include/define flags of a `src/game` translation unit."""
    entries = json.loads(compile_commands.read_text())
    game_dir = (src / HEADER_DIR).resolve()
    for entry in entries:
        if Path(entry["file"]).resolve().parent != game_dir:
            continue
        args = entry.get("arguments") or shlex.split(entry["command"])
        out = [args[0]]
        i = 1
        while i < len(args):
            arg = args[i]
            if arg in PAIRED_FLAGS and i + 1 < len(args):
                out += [arg, args[i + 1]]
                i += 2
                continue
            if arg.startswith(KEEP_FLAGS):
                out.append(arg)
            i += 1
        return out
    sys.exit(f"gen_reflection: no {HEADER_DIR}/ translation unit in {compile_commands}")


def dump_ast(command: list[str], src: Path, out: Path) -> list[dict]:
    headers = sorted((src / HEADER_DIR).glob("*.hpp"))
    umbrella = out / "umbrella.cpp"
    umbrella.write_text("".join(f'#include "{h.relative_to(src).as_posix()}"\n' for h in headers))
    proc = subprocess.run(command + ["-fsyntax-only", "-Xclang", "-ast-dump=json",
                                     "-Xclang", f"-ast-dump-filter={ROOT_NAMESPACE}::", str(umbrella)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"gen_reflection: clang failed:\n{proc.stderr[-4000:]}")
    decoder, text, i, objects = json.JSONDecoder(), proc.stdout, 0, []
    while True:  # the filtered dump is a stream of top-level objects, not one document
        while i < len(text) and text[i].isspace():
            i += 1
        if i >= len(text):
            return objects
        obj, i = decoder.raw_decode(text, i)
        objects.append(obj)


def _record(node: dict, scope: str, records: dict[str, Record]) -> None:
    if not node.get("completeDefinition") or not node.get("name") or node.get("tagUsed") == "union":
        return
    name = f"{scope}::{node['name']}"
    record = records.setdefault(name, Record(name))
    access = "private" if node.get("tagUsed") == "class" else "public"
    for child in node.get("inner", []):
        kind = child["kind"]
        if kind == "AccessSpecDecl":
            access = child["access"]
        elif access != "public":
            continue
        elif kind == "FieldDecl":
            qual = child.get("type", {}).get("qualType", "")
            if child.get("name") and not child.get("isBitfield") and "&" not in qual:
                if child["name"] not in record.fields:
                    record.fields.append(child["name"])
        elif kind == "CXXRecordDecl":
            _record(child, name, records)


def collect(objects: list[dict]) -> tuple[dict[str, Record], dict[str, list[Function]]]:
    records: dict[str, Record] = {}
    functions: dict[str, list[Function]] = {}

    def visit(node: dict, scope: str) -> None:
        kind = node["kind"]
        if kind == "NamespaceDecl":
            if node.get("name"):  # an anonymous namespace cannot be named from the host
                for child in node.get("inner", []):
                    visit(child, f"{scope}::{node['name']}")
        elif kind == "CXXRecordDecl":
            _record(node, scope, records)
        elif kind == "FunctionDecl" and node.get("name"):
            inner = node.get("inner", [])
            params = [p.get("name", "") for p in inner if p["kind"] == "ParmVarDecl"]
            functions.setdefault(node["name"], []).append(Function(
                f"{scope}::{node['name']}", params, node["type"]["qualType"],
                any(p["kind"] == "CompoundStmt" for p in inner)))

    for obj in objects:
        visit(obj, f"::{ROOT_NAMESPACE}")
    return {k: v for k, v in records.items() if v.fields}, functions


def citations(src: Path) -> dict[int, tuple[str, Path, bool]]:
    """Original address -> (cited function name, file, whether that site is a definition).

    Only namespace-level comment blocks count (a comment indented inside a body cites
    what that code mirrors, not the function it sits in). An `@port` marker outranks a
    plain `// Ghidra` citation, and a citation at a definition outranks one at a declaration.
    """
    best: dict[int, tuple[int, str, Path, bool]] = {}
    for path in sorted([*src.rglob("*.cpp"), *src.rglob("*.hpp")]):
        lines = path.read_text(errors="replace").splitlines()
        i = 0
        while i < len(lines):
            if not lines[i].startswith("//"):
                i += 1
                continue
            start = i
            while i < len(lines) and lines[i].startswith("//"):
                i += 1
            block = lines[start:i]
            addrs: list[int] = []
            for line in block:
                if m := _PORT_RE.match(line):
                    addrs += [int(a, 16) for a in _ADDR_RE.findall(m["addrs"])]
            marked = bool(addrs)
            if not addrs:
                first = next((m for line in block if (m := _GHIDRA_RE.match(line))), None)
                addrs = [int(first["addr"], 16)] if first else []
            if not addrs:
                continue
            head = " ".join(line.strip() for line in lines[i:i + 3])
            paren = head.find("(")
            if paren < 0:
                continue
            m = _CALL_RE.search(head[:paren + 1])
            if not m or m["name"] in KEYWORDS:
                continue
            rest = head[paren:]
            is_definition = rest.find("{") >= 0 and (rest.find(";") < 0 or rest.find("{") < rest.find(";"))
            rank = 2 * marked + is_definition
            for addr in addrs:
                if addr not in best or rank > best[addr][0]:
                    best[addr] = (rank, m["name"], path, is_definition)
    return {addr: (name, path, is_def) for addr, (_, name, path, is_def) in best.items()}


def defined_in_sources(name: str, src: Path, cache: dict[Path, str]) -> bool:
    """Whether some .cpp defines `name` at namespace level (so taking its address links)."""
    pattern = re.compile(rf"^(?!\s)[^;{{}}\n]*\b{re.escape(name)}\s*\(", re.M)
    for path in src.rglob("*.cpp"):
        text = cache.setdefault(path, path.read_text(errors="replace"))
        for m in pattern.finditer(text):
            tail = text[m.end():m.end() + 2000]
            brace, semi = tail.find("{"), tail.find(";")
            if brace >= 0 and (semi < 0 or brace < semi):
                return True
    return False


def targets(src: Path, functions: dict[str, list[Function]]
            ) -> tuple[list[tuple[int, Function]], list[tuple[int, str, str]]]:
    """The cited functions the host can take the address of, and (addr, name, why not) for the rest."""
    picked: list[tuple[int, Function]] = []
    unbound: list[tuple[int, str, str]] = []
    cache: dict[Path, str] = {}
    for addr, (name, path, is_definition) in sorted(citations(src).items()):
        decls = functions.get(name)
        if not decls:
            unbound.append((addr, name, f"cited in {path.relative_to(src.parent).as_posix()} but not a "
                                        f"namespace-level function declared in a src/{HEADER_DIR} header"))
            continue
        if len({d.signature for d in decls}) > 1:
            unbound.append((addr, name, "overloaded"))
            continue
        decl = next((d for d in decls if all(d.params)), decls[0])
        if not (is_definition or any(d.has_body for d in decls) or defined_in_sources(name, src, cache)):
            unbound.append((addr, name, "declared but not defined"))
            continue
        picked.append((addr, decl))
    return picked, unbound


def _cpp_string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(src: Path, records: dict[str, Record], bound: list[tuple[int, Function]],
           unbound: list[tuple[int, str, str]]) -> tuple[str, str]:
    headers = sorted((src / HEADER_DIR).glob("*.hpp"))
    names = sorted(records)
    hpp = ["// Generated by tools/microexec_evnova/host/gen_reflection.py. Do not edit.", "#pragma once", "",
           '#include "reflect.hpp"', ""]
    hpp += [f'#include "{h.relative_to(src).as_posix()}"' for h in headers]
    hpp += ["", "namespace microexec {", ""]
    for name in names:
        hpp.append(f"template <> struct Fields<{name}> {{\n"
                   f"  static constexpr std::string_view kName = {_cpp_string(name.removeprefix('::'))};\n"
                   f"  static const FieldTable<{name}> &Get();\n}};")
    hpp += ["", "} // namespace microexec", ""]

    cpp = ["// Generated by tools/microexec_evnova/host/gen_reflection.py. Do not edit.", "",
           '#include "port_reflection.gen.hpp"', "",
           "// NOLINTBEGIN",
           "#define MICROEXEC_FIELD(T, f) \\",
           "  {#f, {[](T &o, std::span<const Step> r, const Op &op) { Walk(o.f, r, op); }, &KindOf<decltype(T::f)>, \\",
           "        [](T &o, const std::string &p, LeafRuns &runs) { Collect(o.f, p, runs); }}}",
           "", "namespace microexec {", ""]
    for name in names:
        rows = ",\n".join(f"      MICROEXEC_FIELD({name}, {f})" for f in records[name].fields)
        cpp.append(f"const FieldTable<{name}> &Fields<{name}>::Get() {{\n"
                   f"  static const FieldTable<{name}> table = {{\n{rows},\n  }};\n  return table;\n}}\n")
    cpp.append("void DescribeRecords(std::ostream &out) {")
    cpp += [f"  DescribeRecord<{name}>(out);" for name in names]
    cpp.append("}\n")
    cpp.append("std::span<const UnboundTarget> UnboundTargets() {")
    if unbound:  # a zero-length array is ill-formed
        cpp.append("  static const UnboundTarget targets[] = {")
        cpp += [f"      {{0x{addr:08x}, {_cpp_string(name)}, {_cpp_string(why)}}}," for addr, name, why in unbound]
        cpp += ["  };", "  return targets;"]
    else:
        cpp.append("  return {};")
    cpp += ["}", ""]
    cpp.append("std::span<const Target> Targets() {")
    for i, (_, fn) in enumerate(bound):
        cpp.append(f"  static constexpr std::string_view params_{i}[] = {{"
                   + ", ".join(_cpp_string(p) for p in fn.params) + ("" if fn.params else '""') + "};")
    if not bound:
        cpp += ["  return {};", "}", "", "} // namespace microexec", "// NOLINTEND", ""]
        return "\n".join(hpp), "\n".join(cpp)
    cpp.append("  static const Target targets[] = {")
    for i, (addr, fn) in enumerate(bound):
        count = len(fn.params)
        cpp.append(f"      {{0x{addr:08x}, {_cpp_string(fn.name.removeprefix('::'))}, "
                   f"std::span(params_{i}, {count}), &Run<&{fn.name}>, &Describe<&{fn.name}>}},")
    cpp += ["  };", "  return targets;", "}", "", "} // namespace microexec", "// NOLINTEND", ""]
    return "\n".join(hpp), "\n".join(cpp)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compile-commands", type=Path, required=True)
    parser.add_argument("--src", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    src = args.src.resolve()
    records, functions = collect(dump_ast(compiler_command(args.compile_commands, src), src, args.out))
    bound, unbound = targets(src, functions)
    hpp, cpp = render(src, records, bound, unbound)
    for name, text in (("port_reflection.gen.hpp", hpp), ("port_reflection.gen.cpp", cpp)):
        path = args.out / name
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
    print(f"gen_reflection: {len(records)} records, {sum(len(r.fields) for r in records.values())} fields, "
          f"{len(bound)} targets ({len(unbound)} cited but unbound)")


if __name__ == "__main__":
    main()
