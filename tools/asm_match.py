#!/usr/bin/env python3
"""Rank Windows EV Nova functions against a symbolized Carbon i386 function.

This is a deliberately small, dependency-free binary matching experiment.  It
compares cheap compiler-resistant features: function size, instruction and
branch/call counts, mnemonic histograms/bigrams, and small numeric constants.
It does not modify either binary, the trackers, or Ghidra.

Examples:
  tools/asm_match.py _ScanPlayer
  tools/asm_match.py _ScanPlayer _AIDispatch _PirateWarshipAI
  tools/asm_match.py 0x7e46e --limit 20
  tools/asm_match.py --all --limit 3
  tools/asm_match.py --reverse --choice-margin 0.03 --limit 3 --output analysis/carbon_asm_matches.txt
  tools/asm_match.py _ScanPlayer --explain
"""

from __future__ import annotations

import argparse
import bisect
import heapq
import math
import re
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CARBON = Path("/Applications/EV Nova.app/Contents/MacOS/Ev Nova.original")
DEFAULT_WINDOWS = ROOT / "EV Nova" / "EV Nova.exe"
TRACKERS = (ROOT / "decomp-progress.tsv", ROOT / "decomp-skipped.tsv")

MAC_INSN_RE = re.compile(r"^([0-9a-fA-F]{8})\s+([A-Za-z][A-Za-z0-9.]*)\s*(.*)$")
WIN_INSN_RE = re.compile(
    r"^\s*([0-9a-fA-F]+):(?:\s+[0-9a-fA-F]{2})+\s+([A-Za-z][A-Za-z0-9.]*)\s*(.*)$"
)
HEX_RE = re.compile(r"(?<![A-Za-z0-9_])(?:\$)?(-?0x[0-9a-fA-F]+|-?\d+)")


@dataclass(frozen=True)
class Symbol:
    address: int
    end: int
    name: str


@dataclass
class Fingerprint:
    symbol: Symbol
    instruction_count: int
    calls: int
    branches: int
    mnemonics: Counter[str]
    bigrams: Counter[tuple[str, str]]
    constants: Counter[int]


@dataclass(frozen=True)
class InstructionIndex:
    addresses: tuple[int, ...]
    rows: tuple[tuple[str, str], ...]

    @classmethod
    def from_dict(cls, instructions: dict[int, tuple[str, str]]) -> "InstructionIndex":
        ordered = sorted(instructions.items())
        return cls(tuple(address for address, _ in ordered), tuple(row for _, row in ordered))

    def range(self, start: int, end: int) -> tuple[tuple[str, str], ...]:
        first = bisect.bisect_left(self.addresses, start)
        last = bisect.bisect_left(self.addresses, end, first)
        return self.rows[first:last]


def run(*args: str) -> str:
    try:
        return subprocess.run(args, check=True, text=True, capture_output=True).stdout
    except FileNotFoundError as exc:
        raise SystemExit(f"required command not found: {args[0]}") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.strip() or exc.stdout.strip()
        raise SystemExit(f"{' '.join(args)} failed: {detail}") from exc


def carbon_symbols(binary: Path, final_end: int) -> list[Symbol]:
    text = run("nm", "-arch", "i386", "-n", str(binary))
    entries: list[tuple[int, str]] = []
    for line in text.splitlines():
        match = re.match(r"^([0-9a-fA-F]+)\s+[Tt]\s+(\S+)$", line)
        if match:
            entries.append((int(match.group(1), 16), match.group(2)))
    return boundaries(entries, final_end)


def windows_symbols(final_end: int) -> list[Symbol]:
    by_address: dict[int, str] = {}
    for tracker in TRACKERS:
        for line in tracker.read_text().splitlines()[1:]:
            fields = line.split("\t")
            if len(fields) >= 2 and fields[0].startswith("0x"):
                by_address[int(fields[0], 16)] = fields[1]
    return boundaries(sorted(by_address.items()), final_end)


def boundaries(entries: list[tuple[int, str]], final_end: int) -> list[Symbol]:
    result: list[Symbol] = []
    for index, (address, name) in enumerate(entries):
        end = entries[index + 1][0] if index + 1 < len(entries) else final_end
        if end > address:
            result.append(Symbol(address, end, name))
    return result


def parse_carbon_instructions(binary: Path) -> dict[int, tuple[str, str]]:
    text = run("otool", "-arch", "i386", "-tvV", str(binary))
    result: dict[int, tuple[str, str]] = {}
    for line in text.splitlines():
        match = MAC_INSN_RE.match(line)
        if match:
            result[int(match.group(1), 16)] = (match.group(2), match.group(3))
    return result


def parse_windows_instructions(binary: Path) -> dict[int, tuple[str, str]]:
    text = run("objdump", "-d", "-Mintel", str(binary))
    result: dict[int, tuple[str, str]] = {}
    for line in text.splitlines():
        match = WIN_INSN_RE.match(line)
        if match:
            result[int(match.group(1), 16)] = (match.group(2), match.group(3))
    return result


def normalize_mnemonic(mnemonic: str) -> str:
    mnemonic = mnemonic.lower()
    aliases = {
        "calll": "call", "retl": "ret", "pushl": "push", "popl": "pop",
        "movl": "mov", "movw": "mov", "movb": "mov", "leal": "lea",
        "cmpl": "cmp", "cmpw": "cmp", "cmpb": "cmp", "testl": "test",
        "testw": "test", "testb": "test", "addl": "add", "subl": "sub",
        "imull": "imul", "incl": "inc", "decl": "dec", "shll": "shl",
        "shrl": "shr", "andl": "and", "orl": "or", "xorl": "xor",
    }
    return aliases.get(mnemonic, mnemonic)


def constants_for(mnemonic: str, operands: str) -> list[int]:
    # Direct branch/call targets and absolute addresses are relocations, not
    # semantic constants.  Small immediates and structure offsets are useful.
    if mnemonic == "call" or mnemonic.startswith("j"):
        return []
    values: list[int] = []
    for token in HEX_RE.findall(operands):
        try:
            value = int(token.replace("$", ""), 0)
        except ValueError:
            continue
        if -0x10000 <= value <= 0x10000:
            values.append(value)
    return values


def fingerprint(symbol: Symbol, instructions: InstructionIndex) -> Fingerprint:
    rows = instructions.range(symbol.address, symbol.end)
    mnemonics: Counter[str] = Counter()
    constants: Counter[int] = Counter()
    sequence: list[str] = []
    calls = branches = 0
    for raw_mnemonic, operands in rows:
        mnemonic = normalize_mnemonic(raw_mnemonic)
        sequence.append(mnemonic)
        mnemonics[mnemonic] += 1
        constants.update(constants_for(mnemonic, operands))
        calls += mnemonic == "call"
        branches += mnemonic.startswith("j")
    return Fingerprint(
        symbol=symbol,
        instruction_count=len(rows),
        calls=calls,
        branches=branches,
        mnemonics=mnemonics,
        bigrams=Counter(zip(sequence, sequence[1:])),
        constants=constants,
    )


def cosine(left: Counter, right: Counter) -> float:
    if not left or not right:
        return 0.0
    dot = sum(value * right.get(key, 0) for key, value in left.items())
    lnorm = math.sqrt(sum(value * value for value in left.values()))
    rnorm = math.sqrt(sum(value * value for value in right.values()))
    return dot / (lnorm * rnorm) if lnorm and rnorm else 0.0


def ratio(left: int, right: int) -> float:
    return min(left, right) / max(left, right) if left and right else 0.0


def weighted_jaccard(left: Counter[int], right: Counter[int], weights: dict[int, float]) -> float:
    keys = set(left) | set(right)
    union = sum(weights.get(key, 1.0) for key in keys)
    overlap = sum(weights.get(key, 1.0) for key in set(left) & set(right))
    return overlap / union if union else 0.0


def score(
    left: Fingerprint,
    right: Fingerprint,
    constant_weights: dict[int, float],
) -> tuple[float, tuple[float, ...]]:
    size = ratio(left.symbol.end - left.symbol.address, right.symbol.end - right.symbol.address)
    count = ratio(left.instruction_count, right.instruction_count)
    mnemonic = cosine(left.mnemonics, right.mnemonics)
    bigram = cosine(left.bigrams, right.bigrams)
    constants = weighted_jaccard(left.constants, right.constants, constant_weights)
    calls = ratio(left.calls, right.calls)
    branches = ratio(left.branches, right.branches)
    total = (
        0.14 * size + 0.05 * count + 0.16 * mnemonic + 0.10 * bigram
        + 0.47 * constants + 0.04 * calls + 0.04 * branches
    )
    return total, (size, count, mnemonic, bigram, constants, calls, branches)


def resolve_target(query: str, symbols: list[Symbol]) -> Symbol:
    try:
        address = int(query, 0)
    except ValueError:
        address = None
    matches = [
        symbol for symbol in symbols
        if symbol.address == address or symbol.name == query or symbol.name.lstrip("_") == query.lstrip("_")
    ]
    if not matches:
        raise SystemExit(f"Carbon function not found: {query}")
    if len(matches) > 1:
        names = ", ".join(f"{item.name}@0x{item.address:x}" for item in matches)
        raise SystemExit(f"ambiguous Carbon function: {names}")
    return matches[0]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("functions", nargs="*", metavar="function", help="Carbon symbol name or address")
    parser.add_argument("--all", action="store_true", help="match every Carbon text symbol")
    parser.add_argument(
        "--reverse",
        action="store_true",
        help="report each Windows function followed by its Carbon choices (implies --all)",
    )
    parser.add_argument("--carbon", type=Path, default=DEFAULT_CARBON)
    parser.add_argument("--windows", type=Path, default=DEFAULT_WINDOWS)
    parser.add_argument("--limit", type=int, default=10)
    parser.add_argument(
        "--choice-margin",
        type=float,
        default=0.03,
        help="in reverse reports, hide alternatives when choice 1 leads choice 2 by this much",
    )
    parser.add_argument("--explain", action="store_true", help="show component scores and shared constants")
    parser.add_argument("--output", type=Path, help="write the report to this file instead of stdout")
    args = parser.parse_args()

    if (args.all or args.reverse) and args.functions:
        parser.error("function arguments and --all are mutually exclusive")
    if not args.all and not args.reverse and not args.functions:
        parser.error("provide at least one Carbon function or use --all")
    if args.reverse and args.explain:
        parser.error("--reverse and --explain cannot be combined")
    if args.limit < 1:
        parser.error("--limit must be at least 1")
    if args.choice_margin < 0:
        parser.error("--choice-margin cannot be negative")

    for path in (args.carbon, args.windows, *TRACKERS):
        if not path.exists():
            raise SystemExit(f"not found: {path}")

    mac_instructions = InstructionIndex.from_dict(parse_carbon_instructions(args.carbon))
    mac_symbols = carbon_symbols(args.carbon, mac_instructions.addresses[-1] + 16)
    target_symbols = mac_symbols if args.all or args.reverse else [resolve_target(query, mac_symbols) for query in args.functions]
    targets = [fingerprint(symbol, mac_instructions) for symbol in target_symbols]
    targets = [target for target in targets if target.instruction_count]

    win_instructions = InstructionIndex.from_dict(parse_windows_instructions(args.windows))
    candidates = [
        fingerprint(symbol, win_instructions)
        for symbol in windows_symbols(win_instructions.addresses[-1] + 16)
    ]
    candidates = [candidate for candidate in candidates if candidate.instruction_count]
    constant_df: Counter[int] = Counter()
    for candidate in candidates:
        constant_df.update(candidate.constants.keys())
    constant_weights = {
        value: math.log((len(candidates) + 1) / (frequency + 1)) + 1.0
        for value, frequency in constant_df.items()
    }
    rare_postings: dict[int, set[int]] = defaultdict(set)
    for index, candidate in enumerate(candidates):
        for value in candidate.constants:
            if constant_df[value] <= 64:
                rare_postings[value].add(index)
    size_index = sorted(
        (candidate.symbol.end - candidate.symbol.address, index)
        for index, candidate in enumerate(candidates)
    )
    instruction_index = sorted(
        (candidate.instruction_count, index) for index, candidate in enumerate(candidates)
    )

    def shortlist(target: Fingerprint) -> list[Fingerprint]:
        indices: set[int] = set()
        for value in target.constants:
            indices.update(rare_postings.get(value, ()))
        for index, sought in (
            (size_index, target.symbol.end - target.symbol.address),
            (instruction_index, target.instruction_count),
        ):
            position = bisect.bisect_left(index, (sought, -1))
            indices.update(item[1] for item in index[max(0, position - 64):position + 64])
        return [candidates[index] for index in indices]

    def plausible_matches(target: Fingerprint):
        return (
            candidate for candidate in shortlist(target)
            if ratio(
                target.symbol.end - target.symbol.address,
                candidate.symbol.end - candidate.symbol.address,
            ) >= 0.35
            and ratio(target.instruction_count, candidate.instruction_count) >= 0.30
        )

    if args.reverse:
        choices: dict[int, list[tuple[float, int, Fingerprint]]] = defaultdict(list)
        for target in targets:
            for candidate in plausible_matches(target):
                total = score(target, candidate, constant_weights)[0]
                heap = choices[candidate.symbol.address]
                item = (total, target.symbol.address, target)
                if len(heap) < args.limit:
                    heapq.heappush(heap, item)
                elif item[:2] > heap[0][:2]:
                    heapq.heapreplace(heap, item)
        lines = []
        for candidate in candidates:
            ranked_choices = sorted(choices[candidate.symbol.address], reverse=True)
            if (
                len(ranked_choices) >= 2
                and ranked_choices[0][0] - ranked_choices[1][0] >= args.choice_margin
            ):
                ranked_choices = ranked_choices[:1]
            rendered = " / ".join(
                f"{target.symbol.name} [0x{target.symbol.address:08x}] ({total:.4f})"
                for total, _, target in ranked_choices
            )
            lines.append(
                f"{candidate.symbol.name} [0x{candidate.symbol.address:08x}] -> "
                f"{rendered or '(no candidate)'}"
            )
        report = "\n".join(lines) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(report)
        else:
            sys.stdout.write(report)
        return 0

    labels = ("size", "insns", "mnemonic", "bigram", "constants", "calls", "branches")
    for target_index, target in enumerate(targets):
        ranked = heapq.nlargest(
            args.limit,
            ((score(target, candidate, constant_weights), candidate) for candidate in plausible_matches(target)),
            key=lambda item: item[0][0],
        )
        if target_index:
            print()
        print(
            f"Carbon {target.symbol.name} 0x{target.symbol.address:08x}: "
            f"{target.symbol.end - target.symbol.address} bytes, "
            f"{target.instruction_count} instructions"
        )
        for rank, ((total, parts), candidate) in enumerate(ranked, 1):
            symbol = candidate.symbol
            line = (
                f"{rank:2d}. {total:.4f}  0x{symbol.address:08x}  {symbol.name} "
                f"({symbol.end - symbol.address} bytes, {candidate.instruction_count} insns)"
            )
            print(line)
            if args.explain:
                detail = " ".join(f"{label}={value:.3f}" for label, value in zip(labels, parts))
                shared = sorted(
                    set(target.constants) & set(candidate.constants),
                    key=lambda value: (abs(value), value),
                )
                constants = ", ".join(hex(value) if value >= 0 else str(value) for value in shared[-20:])
                print(f"    {detail}")
                print(f"    shared constants: {constants or '(none)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
