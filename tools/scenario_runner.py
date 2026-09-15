#!/usr/bin/env python3
"""Run semantic EV Nova probe scenarios described by TOML files."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
import time
import tomllib
import urllib.error
import urllib.parse
import urllib.request


class ScenarioError(RuntimeError):
    pass


class Probe:
    def __init__(self, base_url: str) -> None:
        self.base_url = base_url.rstrip("/")

    def get(self, path: str) -> object:
        with urllib.request.urlopen(self.base_url + path, timeout=10) as response:
            return json.loads(response.read())

    def post(self, path: str, payload: dict[str, object]) -> object:
        request = urllib.request.Request(
            self.base_url + path,
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.loads(response.read())

    def screenshot(self) -> bytes:
        with urllib.request.urlopen(
            self.base_url + "/probe/screenshot", timeout=10
        ) as response:
            return response.read()


def nested_value(value: object, path: list[str]) -> object:
    for component in path:
        if isinstance(value, dict) and component in value:
            value = value[component]
        elif isinstance(value, list) and component.isdigit():
            index = int(component)
            if index >= len(value):
                raise KeyError(".".join(path))
            value = value[index]
        else:
            raise KeyError(".".join(path))
    return value


def observations(probe: Probe, expected: dict[str, object]) -> dict[str, object]:
    roots = {key.split(".", 1)[0] for key in expected}
    result: dict[str, object] = {}
    if "ui" in roots:
        result["ui"] = probe.get("/probe/ui")
    if "state" in roots:
        result["state"] = probe.get("/probe/state?query=summary")
    if "travel" in roots:
        result["travel"] = probe.get("/probe/state?query=travel")
    if "missions" in roots:
        result["missions"] = probe.get("/probe/state?query=missions")
    if "automation" in roots:
        result["automation"] = probe.get("/probe/automation")
    unknown = roots - result.keys()
    if unknown:
        raise ScenarioError(f"unknown observation root(s): {', '.join(sorted(unknown))}")
    return result


def matches(observed: dict[str, object], expected: dict[str, object]) -> bool:
    for path, wanted in expected.items():
        try:
            actual = nested_value(observed, path.split("."))
        except KeyError:
            return False
        if actual != wanted:
            return False
    return True


def validate_state(
    probe: Probe,
    expected: dict[str, object],
    regex_matches: dict[str, object],
) -> None:
    paths = set(expected) | set(regex_matches)
    if not paths:
        raise ScenarioError("validate_state requires expect and/or matches")
    observed = observations(probe, {path: None for path in paths})
    failures: list[str] = []
    for path, wanted in expected.items():
        try:
            actual = nested_value(observed, path.split("."))
        except KeyError:
            failures.append(f"{path}: path is absent")
            continue
        if actual != wanted:
            failures.append(f"{path}: expected {wanted!r}, got {actual!r}")
    for path, pattern in regex_matches.items():
        if not isinstance(pattern, str):
            raise ScenarioError(f"validate_state regex for {path} must be a string")
        try:
            actual = nested_value(observed, path.split("."))
        except KeyError:
            failures.append(f"{path}: path is absent")
            continue
        if re.fullmatch(pattern, str(actual)) is None:
            failures.append(f"{path}: {actual!r} does not match /{pattern}/")
    if failures:
        raise ScenarioError("state validation failed: " + "; ".join(failures))


def wait_for(
    probe: Probe, expected: dict[str, object], timeout_ms: int
) -> dict[str, object]:
    deadline = time.monotonic() + timeout_ms / 1000
    last: dict[str, object] = {}
    while time.monotonic() < deadline:
        last = observations(probe, expected)
        if matches(last, expected):
            return last
        automation = last.get("automation")
        if isinstance(automation, dict) and automation.get("phase") == "failed":
            raise ScenarioError(f"automation failed: {automation.get('detail', '')}")
        time.sleep(0.05)
    raise ScenarioError(
        f"timed out waiting for {expected!r}; last observation: {last!r}"
    )


def click_when_available(probe: Probe, element: object, timeout_ms: int) -> None:
    if not isinstance(element, str) or not element:
        raise ScenarioError("click requires a non-empty element")
    deadline = time.monotonic() + timeout_ms / 1000
    while time.monotonic() < deadline:
        ui = probe.get("/probe/ui")
        if isinstance(ui, dict) and element in ui.get("rects", {}):
            try:
                probe.post("/probe/click", {"element": element})
                return
            except urllib.error.HTTPError as error:
                if error.code != 409:
                    raise
        time.sleep(0.02)
    raise ScenarioError(f"timed out waiting to click UI element {element!r}")


def capture_diagnostics(probe: Probe, directory: Path, step_number: int) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    snapshot: dict[str, object] = {"failed_step": step_number}
    endpoints = {
        "ui": "/probe/ui",
        "state": "/probe/state?query=summary",
        "travel": "/probe/state?query=travel",
        "missions": "/probe/state?query=missions",
        "automation": "/probe/automation",
        "logs": "/probe/logs?since=0",
    }
    for name, endpoint in endpoints.items():
        try:
            snapshot[name] = probe.get(endpoint)
        except Exception as error:  # Diagnostics must not hide the first failure.
            snapshot[name] = {"diagnostic_error": str(error)}
    (directory / "failure.json").write_text(
        json.dumps(snapshot, indent=2) + "\n", encoding="utf-8"
    )
    try:
        (directory / "failure.bmp").write_bytes(probe.screenshot())
    except Exception as error:
        (directory / "screenshot-error.txt").write_text(str(error), encoding="utf-8")


def run_step(
    probe: Probe, step: dict[str, object], default_timeout_ms: int, artifacts: Path
) -> None:
    action = step.get("action")
    allowed = {
        "wait": {"action", "expect", "timeout_ms"},
        "validate_state": {"action", "expect", "matches"},
        "click": {"action", "element", "timeout_ms"},
        "key": {"action", "key"},
        "command": {"action", "cmd", "target", "timeout_ms", "enabled", "speed_multiplier", "suppress_audio"},
        "screenshot": {"action", "name"},
        "quit": {"action"},
    }
    if action not in allowed:
        raise ScenarioError(f"unknown action {action!r}")
    unknown = set(step) - allowed[str(action)]
    if unknown:
        raise ScenarioError(f"unknown {action} field(s): {', '.join(sorted(unknown))}")
    if action == "wait":
        expected = step.get("expect")
        if not isinstance(expected, dict) or not expected:
            raise ScenarioError("wait requires a non-empty expect table")
        wait_for(probe, expected, int(step.get("timeout_ms", default_timeout_ms)))
    elif action == "validate_state":
        expected = step.get("expect", {})
        regex_matches = step.get("matches", {})
        if not isinstance(expected, dict) or not isinstance(regex_matches, dict):
            raise ScenarioError("validate_state expect and matches must be tables")
        validate_state(probe, expected, regex_matches)
    elif action == "click":
        click_when_available(
            probe,
            step.get("element"),
            int(step.get("timeout_ms", default_timeout_ms)),
        )
    elif action == "key":
        probe.post("/probe/key", {"key": step["key"]})
    elif action == "command":
        probe.post(
            "/probe/command", {key: value for key, value in step.items() if key != "action"}
        )
    elif action == "screenshot":
        name = str(step.get("name", "screenshot"))
        if Path(name).name != name:
            raise ScenarioError("screenshot name must not contain a directory")
        artifacts.mkdir(parents=True, exist_ok=True)
        (artifacts / f"{name}.bmp").write_bytes(probe.screenshot())
    elif action == "quit":
        probe.post("/probe/command", {"cmd": "quit"})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", type=Path)
    parser.add_argument("--base-url", default="http://127.0.0.1:8190")
    parser.add_argument("--artifacts", type=Path, default=Path("build/scenario-results"))
    args = parser.parse_args()

    probe = Probe(args.base_url)
    quit_on_finish = False
    try:
        document = tomllib.loads(args.scenario.read_text(encoding="utf-8"))
        if document.get("version") != 1:
            raise ScenarioError("scenario version must be 1")
        name = document.get("name")
        steps = document.get("steps")
        if not isinstance(name, str) or not name or not isinstance(steps, list):
            raise ScenarioError("scenario requires a name and [[steps]]")
        quit_on_finish = document.get("quit_on_finish", False)
        if not isinstance(quit_on_finish, bool):
            raise ScenarioError("quit_on_finish must be true or false")
        defaults = document.get("defaults", {})
        timeout_ms = int(defaults.get("timeout_ms", 30_000))
        artifact_dir = args.artifacts / name
        for index, step in enumerate(steps, 1):
            if not isinstance(step, dict):
                raise ScenarioError(f"step {index} is not a table")
            print(f"[{index}/{len(steps)}] {step.get('action', '<missing>')}", flush=True)
            try:
                run_step(probe, step, timeout_ms, artifact_dir)
            except Exception:
                capture_diagnostics(probe, artifact_dir, index)
                raise
        print(f"scenario {name!r} passed")
        return 0
    except (OSError, tomllib.TOMLDecodeError, urllib.error.URLError, ScenarioError, KeyError) as error:
        print(f"scenario failed: {error}", file=sys.stderr)
        return 1
    finally:
        if quit_on_finish:
            try:
                probe.post("/probe/command", {"cmd": "quit"})
            except Exception as error:
                print(f"warning: could not quit probe target: {error}", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())
