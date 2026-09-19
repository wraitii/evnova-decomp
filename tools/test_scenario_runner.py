#!/usr/bin/env python3
"""Focused regression tests for scenario composition (callable fragments)."""

import tempfile
from pathlib import Path
import unittest

import scenario_runner as runner


def _write(directory: Path, name: str, text: str) -> Path:
    path = directory / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class LoadScenarioTest(unittest.TestCase):
    def setUp(self) -> None:
        runner.clear_scenario_cache()

    def test_params_and_defaults_parse(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = _write(
                Path(tmp),
                "fragment.toml",
                """
version = 1
name = "frag"
[defaults]
timeout_ms = 12000
[[params]]
name = "system"
[[params]]
name = "timeout_ms"
default = 180000
[[steps]]
action = "quit"
""",
            )
            scenario = runner.load_scenario(path)
            self.assertEqual(scenario.name, "frag")
            self.assertEqual(scenario.timeout_ms, 12000)
            self.assertEqual(
                [(p.name, p.has_default) for p in scenario.params],
                [("system", False), ("timeout_ms", True)],
            )

    def test_rejects_bad_param_declarations(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            duplicate = _write(
                Path(tmp),
                "dup.toml",
                """
version = 1
name = "dup"
[[params]]
name = "x"
[[params]]
name = "x"
[[steps]]
action = "quit"
""",
            )
            with self.assertRaises(runner.ScenarioError):
                runner.load_scenario(duplicate)


class BindArgsTest(unittest.TestCase):
    def _scenario(self, directory: Path):
        path = _write(
            directory,
            "frag.toml",
            """
version = 1
name = "frag"
[[params]]
name = "system"
[[params]]
name = "extra"
default = "fallback"
[[steps]]
action = "quit"
""",
        )
        return runner.load_scenario(path)

    def test_defaults_fill_omitted_args(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bound = runner.bind_args(self._scenario(Path(tmp)), {"system": "Sol"})
            self.assertEqual(bound, {"system": "Sol", "extra": "fallback"})

    def test_rejects_unknown_and_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            scenario = self._scenario(Path(tmp))
            with self.assertRaises(runner.ScenarioError):
                runner.bind_args(scenario, {"system": "Sol", "typo": 1})
            with self.assertRaises(runner.ScenarioError):
                runner.bind_args(scenario, {})


class SubstituteTest(unittest.TestCase):
    source = Path("frag.toml")

    def test_whole_placeholder_keeps_type(self) -> None:
        params = {"n": 10, "flag": True, "name": "Sol"}
        self.assertEqual(runner.substitute("{{ n }}", params, self.source), 10)
        self.assertIs(runner.substitute("{{flag}}", params, self.source), True)
        self.assertEqual(
            runner.substitute("{{ name }}", params, self.source), "Sol"
        )

    def test_embedded_placeholder_is_stringified(self) -> None:
        params = {"n": 10}
        self.assertEqual(
            runner.substitute("count-{{ n }}", params, self.source), "count-10"
        )

    def test_recurses_into_tables_and_arrays(self) -> None:
        params = {"system": "Sol", "n": 3}
        step = {
            "action": "wait",
            "expect": {"state.system": "{{ system }}", "cargo.bins.{{ n }}.tons": 0},
            "items": ["{{ n }}", "static"],
        }
        self.assertEqual(
            runner.substitute(step, params, self.source),
            {
                "action": "wait",
                "expect": {"state.system": "Sol", "cargo.bins.3.tons": 0},
                "items": [3, "static"],
            },
        )

    def test_undefined_placeholder_fails(self) -> None:
        with self.assertRaises(runner.ScenarioError):
            runner.substitute("{{ missing }}", {}, self.source)


class ResolveScenarioPathTest(unittest.TestCase):
    def test_adds_toml_suffix_and_resolves_relative(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            resolved = runner.resolve_scenario_path("fragments/travel", base)
            self.assertEqual(resolved, (base / "fragments/travel.toml").resolve())
            already = runner.resolve_scenario_path("frag.toml", base)
            self.assertEqual(already, (base / "frag.toml").resolve())


class FakeProbe:
    def __init__(self, summary: dict[str, object]) -> None:
        self.summary = summary
        self.gets: list[str] = []

    def get(self, path: str) -> object:
        self.gets.append(path)
        if path.startswith("/probe/state?query=summary"):
            return self.summary
        raise AssertionError(f"unexpected GET {path}")

    def post(self, path: str, payload: dict[str, object]) -> object:
        raise AssertionError(f"unexpected POST {path} {payload}")

    def screenshot(self) -> bytes:
        return b""


class UiProbe:
    """Fake probe whose /probe/ui returns one fixed snapshot."""

    def __init__(self, ui: dict[str, object]) -> None:
        self.ui = ui
        self.gets: list[str] = []

    def get(self, path: str) -> object:
        self.gets.append(path)
        if path == "/probe/ui":
            return self.ui
        raise AssertionError(f"unexpected GET {path}")

    def post(self, path: str, payload: dict[str, object]) -> object:
        raise AssertionError(f"unexpected POST {path} {payload}")

    def screenshot(self) -> bytes:
        return b""


class WaitAnyTest(unittest.TestCase):
    def _context(self, probe: object) -> runner.RunContext:
        return runner.RunContext(
            probe=probe,
            artifacts=Path("artifacts"),
            base_dir=Path("."),
            stack=(),
        )

    def test_expect_any_matches_second_alternative(self) -> None:
        probe = UiProbe({"window": "ui_dialog_ditl_3001"})
        step = {
            "action": "wait",
            "expect_any": [
                {"ui.window": "ui_dialog_ditl_3002"},
                {"ui.window": "ui_dialog_ditl_3001"},
            ],
        }
        runner.run_step(self._context(probe), step, 1000, "1")

    def test_expect_and_expect_any_are_mutually_exclusive(self) -> None:
        probe = UiProbe({"window": "ui_dialog_ditl_3001"})
        step = {
            "action": "wait",
            "expect": {"ui.window": "ui_dialog_ditl_3001"},
            "expect_any": [{"ui.window": "ui_dialog_ditl_3001"}],
        }
        with self.assertRaises(runner.ScenarioError):
            runner.run_step(self._context(probe), step, 1000, "1")

    def test_expect_any_rejects_bad_shape(self) -> None:
        probe = UiProbe({"window": "ui_dialog_ditl_3001"})
        for bad in ([], [None], [{}], "nope"):
            step = {"action": "wait", "expect_any": bad}
            with self.assertRaises(runner.ScenarioError):
                runner.run_step(self._context(probe), step, 1000, "1")


class CallIntegrationTest(unittest.TestCase):
    def setUp(self) -> None:
        runner.clear_scenario_cache()
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _run(self, probe: FakeProbe, step: dict[str, object]) -> None:
        context = runner.RunContext(
            probe=probe,
            artifacts=self.base / "artifacts",
            base_dir=self.base,
            stack=(),
        )
        runner.run_step(context, step, 1000, "1")

    def test_call_binds_and_substitutes_arguments(self) -> None:
        _write(
            self.base,
            "land.toml",
            """
version = 1
name = "land"
[[params]]
name = "target"
[[steps]]
action = "validate_state"
expect = { "state.system" = "{{ target }}" }
""",
        )
        probe = FakeProbe({"system": "Sol"})
        self._run(probe, {"action": "call", "scenario": "land", "args": {"target": "Sol"}})
        self.assertEqual(len(probe.gets), 1)

    def test_nested_calls_forward_parameters(self) -> None:
        _write(
            self.base,
            "land.toml",
            """
version = 1
name = "land"
[[params]]
name = "target"
[[steps]]
action = "validate_state"
expect = { "state.system" = "{{ target }}" }
""",
        )
        _write(
            self.base,
            "route.toml",
            """
version = 1
name = "route"
[[params]]
name = "destination"
[[steps]]
action = "call"
scenario = "land"
args = { target = "{{ destination }}" }
""",
        )
        probe = FakeProbe({"system": "Sol"})
        self._run(
            probe,
            {"action": "call", "scenario": "route", "args": {"destination": "Sol"}},
        )
        self.assertEqual(len(probe.gets), 1)

    def test_recursive_call_is_rejected(self) -> None:
        loop = _write(
            self.base,
            "loop.toml",
            """
version = 1
name = "loop"
[[steps]]
action = "call"
scenario = "loop"
""",
        )
        context = runner.RunContext(
            probe=FakeProbe({}),
            artifacts=self.base / "artifacts",
            base_dir=self.base,
            stack=(loop.resolve(),),
        )
        step = {"action": "call", "scenario": "loop"}
        with self.assertRaises(runner.ScenarioError):
            runner.run_step(context, step, 1000, "1")


if __name__ == "__main__":
    unittest.main()
