#!/usr/bin/env python3
"""Tests for the reusable embedding helpers and duplicate-function bookkeeping."""

import io
import unittest
from pathlib import Path

import cpp_symbols
import find_duplicate_functions as dupes
import semantic_embed


class WriteLinesTest(unittest.TestCase):
    def test_writes_lines_atomically_with_trailing_newline(self) -> None:
        buffer = io.StringIO()
        semantic_embed.write_lines(["a", "b"], file=buffer)
        self.assertEqual(buffer.getvalue(), "a\nb\n")

    def test_empty_input_writes_nothing(self) -> None:
        buffer = io.StringIO()
        semantic_embed.write_lines([], file=buffer)
        self.assertEqual(buffer.getvalue(), "")


class RankByBestQueryTest(unittest.TestCase):
    def test_picks_best_query_index(self) -> None:
        scores = [[0.1, 0.9, 0.3], [0.5, 0.2, 0.4], [0.0, 0.0, 0.7]]
        self.assertEqual(
            semantic_embed.rank_by_best_query(scores),
            [(0.9, 1), (0.5, 0), (0.7, 2)],
        )


class DedupePairsTest(unittest.TestCase):
    def test_keeps_highest_score_per_unordered_pair(self) -> None:
        units = [
            dupes.FunctionUnit(Path("a.c"), "a", "a"),
            dupes.FunctionUnit(Path("b.c"), "b", "b"),
            dupes.FunctionUnit(Path("c.c"), "c", "c"),
        ]
        pairs = [(0, 1, 0.8), (1, 0, 0.9), (0, 2, 0.7)]
        ranked = dupes.dedupe_pairs(pairs, units, limit=10)
        self.assertEqual(
            [(score, left.label, right.label) for score, left, right in ranked],
            [(0.9, "a", "b"), (0.7, "a", "c")],
        )


class CppSymbolsTest(unittest.TestCase):
    def test_qualified_collect_skips_declarations(self) -> None:
        lines = [
            "class Foo {",
            " public:",
            "  void bar() {",
            "    x();",
            "  }",
            "  void decl_only();",
            "};",
        ]
        symbols = [
            {
                "name": "Foo",
                "kind": 5,
                "range": {"start": {"line": 0}, "end": {"line": 6}},
                "children": [
                    {
                        "name": "bar",
                        "kind": 6,
                        "range": {"start": {"line": 2}, "end": {"line": 4}},
                    },
                    {
                        "name": "decl_only",
                        "kind": 6,
                        "range": {"start": {"line": 5}, "end": {"line": 5}},
                    },
                ],
            }
        ]
        out: list[cpp_symbols.CppFunction] = []
        cpp_symbols._collect(symbols, Path("x.hpp"), lines, "", out)
        self.assertEqual([f.name for f in out], ["Foo::bar"])
        self.assertEqual(out[0].line, 3)


if __name__ == "__main__":
    unittest.main()
