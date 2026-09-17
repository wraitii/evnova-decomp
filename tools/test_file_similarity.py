#!/usr/bin/env python3
"""Tests for the model-free file-similarity aggregation helpers."""

import unittest
from pathlib import Path

import numpy as np

import file_similarity as fs


class L2NormalizeTest(unittest.TestCase):
    def test_normalizes_rows(self) -> None:
        matrix = fs.l2_normalize(np.array([[3.0, 4.0], [0.0, 2.0]]))
        np.testing.assert_allclose(matrix, [[0.6, 0.8], [0.0, 1.0]])

    def test_leaves_zero_rows(self) -> None:
        matrix = fs.l2_normalize(np.array([[0.0, 0.0]]))
        np.testing.assert_allclose(matrix, [[0.0, 0.0]])

    def test_normalizes_vector(self) -> None:
        vector = fs.l2_normalize(np.array([0.0, 5.0]))
        np.testing.assert_allclose(vector, [0.0, 1.0])


class GroupByFileTest(unittest.TestCase):
    def test_groups_indices_preserving_order(self) -> None:
        paths = [Path("a"), Path("b"), Path("a")]
        self.assertEqual(
            fs.group_by_file(paths), {Path("a"): [0, 2], Path("b"): [1]}
        )


class HeaderSiblingTest(unittest.TestCase):
    def test_source_and_header_with_same_stem(self) -> None:
        self.assertTrue(fs.is_header_sibling(Path("x/foo.cpp"), Path("x/foo.hpp")))
        self.assertTrue(fs.is_header_sibling(Path("foo.h"), Path("foo.cc")))

    def test_different_stem_or_directory(self) -> None:
        self.assertFalse(fs.is_header_sibling(Path("x/foo.cpp"), Path("x/bar.hpp")))
        self.assertFalse(fs.is_header_sibling(Path("x/foo.cpp"), Path("y/foo.hpp")))

    def test_two_sources_are_not_siblings(self) -> None:
        self.assertFalse(fs.is_header_sibling(Path("foo.cpp"), Path("bar.cpp")))


class FileVectorsTest(unittest.TestCase):
    def test_pools_and_normalizes(self) -> None:
        vectors = np.array([[1.0, 0.0], [1.0, 0.0], [0.0, 1.0]])
        groups = {Path("a"): [0, 1], Path("b"): [2]}
        files, pooled = fs.file_vectors(vectors, groups)
        self.assertEqual(files, [Path("a"), Path("b")])
        np.testing.assert_allclose(np.linalg.norm(pooled, axis=1), [1.0, 1.0])
        np.testing.assert_allclose(pooled[0], [1.0, 0.0])

    def test_similar_files_have_high_cosine(self) -> None:
        vectors = np.array([[1.0, 0.0], [0.9, 0.1], [1.0, 0.0], [0.8, 0.2]])
        files, pooled = fs.file_vectors(
            vectors, {Path("a"): [0, 1], Path("b"): [2, 3]}
        )
        similarity = pooled @ pooled.T
        self.assertGreater(similarity[0, 1], 0.99)


class CohesionScoresTest(unittest.TestCase):
    def test_cohesive_file_scores_high(self) -> None:
        vectors = np.array([[1.0, 0.0], [0.99, 0.01]])
        scores = fs.cohesion_scores(vectors, {Path("a"): [0, 1]})
        self.assertGreater(scores[Path("a")], 0.99)

    def test_scattered_file_scores_low(self) -> None:
        vectors = np.array([[1.0, 0.0], [0.0, 1.0]])
        scores = fs.cohesion_scores(vectors, {Path("a"): [0, 1]})
        self.assertAlmostEqual(scores[Path("a")], 0.0)

    def test_singleton_file_is_skipped(self) -> None:
        vectors = np.array([[1.0, 0.0]])
        self.assertEqual(fs.cohesion_scores(vectors, {Path("a"): [0]}), {})


class BestExternalMatchTest(unittest.TestCase):
    def test_matches_across_files_only(self) -> None:
        vectors = np.array([[1.0, 0.0], [0.9, 0.1], [1.0, 0.0]])
        paths = [Path("a"), Path("a"), Path("b")]
        matches = fs.best_external_match(vectors, paths)
        self.assertEqual(matches[0][1], 2)
        self.assertGreater(matches[0][2], 0.99)

    def test_similar_function_in_other_file(self) -> None:
        vectors = np.array([[1.0, 0.0], [0.8, 0.2], [0.0, 1.0], [1.0, 0.0]])
        paths = [Path("a"), Path("a"), Path("b"), Path("b")]
        matches = fs.best_external_match(vectors, paths)
        self.assertEqual(matches[0][1], 3)

    def test_dominant_external_file(self) -> None:
        vectors = np.array([[1.0, 0.0], [1.0, 0.0], [1.0, 0.0], [0.0, 1.0]])
        paths = [Path("a"), Path("a"), Path("b"), Path("c")]
        matches = fs.best_external_match(vectors, paths)
        destination, count = fs.dominant_external_file([0, 1], matches, paths)
        self.assertEqual(destination, Path("b"))
        self.assertEqual(count, 2)

    def test_skips_header_sibling(self) -> None:
        vectors = np.array([[1.0, 0.0], [1.0, 0.0], [0.7, 0.7]])
        paths = [Path("foo.cpp"), Path("foo.hpp"), Path("bar.cpp")]
        matches = fs.best_external_match(vectors, paths)
        self.assertEqual(matches[0][1], 2)

    def test_unmatched_function_has_minus_one(self) -> None:
        vectors = np.array([[1.0, 0.0], [1.0, 0.0]])
        paths = [Path("foo.cpp"), Path("foo.hpp")]
        matches = fs.best_external_match(vectors, paths)
        self.assertEqual(matches[0][1], -1)
        self.assertEqual(fs.dominant_external_file([0], matches, paths), (None, 0))


class FilePairsTest(unittest.TestCase):
    def test_ranks_and_filters(self) -> None:
        pooled = np.array([[1.0, 0.0], [1.0, 0.0], [0.0, 1.0]])
        files = [Path("a"), Path("b"), Path("c")]
        pairs = fs.file_similarity_pairs(files, pooled, threshold=0.5)
        self.assertEqual(len(pairs), 1)
        self.assertEqual({pairs[0][1], pairs[0][2]}, {Path("a"), Path("b")})

    def test_skips_header_sibling(self) -> None:
        pooled = np.array([[1.0, 0.0], [1.0, 0.0], [0.0, 1.0]])
        files = [Path("foo.cpp"), Path("foo.hpp"), Path("bar.cpp")]
        self.assertEqual(fs.file_similarity_pairs(files, pooled, 0.5), [])

    def test_can_include_header_siblings(self) -> None:
        pooled = np.array([[1.0, 0.0], [1.0, 0.0], [0.0, 1.0]])
        files = [Path("foo.cpp"), Path("foo.hpp"), Path("bar.cpp")]
        pairs = fs.file_similarity_pairs(files, pooled, 0.5, skip_siblings=False)
        self.assertEqual(len(pairs), 1)


class IntraFilePairsTest(unittest.TestCase):
    def test_only_same_file_pairs(self) -> None:
        vectors = np.array([[1.0, 0.0], [1.0, 0.0], [0.9, 0.1], [0.0, 1.0]])
        groups = {Path("a"): [0, 1], Path("b"): [2, 3]}
        pairs = fs.intra_file_pairs(vectors, groups, top_k=10, threshold=0.0)
        self.assertEqual([(left, right) for _, left, right in pairs], [(0, 1), (2, 3)])

    def test_respects_threshold(self) -> None:
        vectors = np.array([[1.0, 0.0], [0.0, 1.0]])
        groups = {Path("a"): [0, 1]}
        self.assertEqual(fs.intra_file_pairs(vectors, groups, 10, 0.5), [])


class LineCountTest(unittest.TestCase):
    def test_counts_lines_without_trailing_newline(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "x.cpp"
            path.write_text("a\nb\nc\n")
            self.assertEqual(fs.line_count(path), 3)

    def test_missing_file_returns_zero(self) -> None:
        self.assertEqual(fs.line_count(Path("/nonexistent/does-not-exist.cpp")), 0)


if __name__ == "__main__":
    unittest.main()
