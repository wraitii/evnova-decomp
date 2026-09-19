#!/usr/bin/env python3
"""Run semantic EV Nova probe scenarios described by TOML files."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
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


class StepFailure(ScenarioError):
    """A step error whose diagnostics were captured under its own label.

    Raised by the innermost ``run_steps`` so an enclosing ``repeat`` does not
    overwrite the failing nested step's diagnostics with its own label.
    """

    def __init__(self, label: str, error: Exception) -> None:
        super().__init__(f"step {label}: {error}")
        self.label = label
        self.error = error


# A called scenario (a reusable "function") may declare parameters. A
# parameter without a default is required; ``args`` must name declared
# parameters exactly, so typos fail instead of being silently ignored.
_NO_DEFAULT = object()
_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_MAX_CALL_DEPTH = 32


@dataclass(frozen=True)
class ScenarioParam:
    name: str
    default: object = _NO_DEFAULT

    @property
    def has_default(self) -> bool:
        return self.default is not _NO_DEFAULT


@dataclass
class Scenario:
    path: Path
    name: str
    steps: list[object]
    params: tuple[ScenarioParam, ...]
    timeout_ms: int
    quit_on_finish: bool


@dataclass
class RunContext:
    probe: Probe
    artifacts: Path
    base_dir: Path
    stack: tuple[Path, ...]


_scenario_cache: dict[Path, Scenario] = {}


def clear_scenario_cache() -> None:
    """Drop parsed-scenario state (tests that rewrite a file between loads)."""
    _scenario_cache.clear()


def _parse_params(raw: object, path: Path) -> tuple[ScenarioParam, ...]:
    if raw is None:
        return ()
    if not isinstance(raw, list):
        raise ScenarioError(f"{path}: params must be an array of tables")
    params: list[ScenarioParam] = []
    seen: set[str] = set()
    for index, entry in enumerate(raw, 1):
        if not isinstance(entry, dict):
            raise ScenarioError(f"{path}: params[{index}] must be a table")
        unknown = set(entry) - {"name", "default"}
        if unknown:
            raise ScenarioError(
                f"{path}: params[{index}] unknown field(s): {', '.join(sorted(unknown))}"
            )
        name = entry.get("name")
        if not isinstance(name, str) or not _IDENTIFIER.fullmatch(name):
            raise ScenarioError(f"{path}: params[{index}] has an invalid name")
        if name in seen:
            raise ScenarioError(f"{path}: duplicate param {name!r}")
        seen.add(name)
        params.append(ScenarioParam(name, entry.get("default", _NO_DEFAULT)))
    return tuple(params)


def load_scenario(path: Path) -> Scenario:
    """Parse and validate a scenario or callable fragment TOML file."""
    resolved = path.resolve()
    cached = _scenario_cache.get(resolved)
    if cached is not None:
        return cached
    document = tomllib.loads(resolved.read_text(encoding="utf-8"))
    if document.get("version") != 1:
        raise ScenarioError(f"{resolved}: scenario version must be 1")
    name = document.get("name")
    steps = document.get("steps")
    if not isinstance(name, str) or not name or not isinstance(steps, list):
        raise ScenarioError(f"{resolved}: scenario requires a name and [[steps]]")
    quit_on_finish = document.get("quit_on_finish", False)
    if not isinstance(quit_on_finish, bool):
        raise ScenarioError(f"{resolved}: quit_on_finish must be true or false")
    defaults = document.get("defaults", {})
    if not isinstance(defaults, dict):
        raise ScenarioError(f"{resolved}: defaults must be a table")
    scenario = Scenario(
        path=resolved,
        name=name,
        steps=steps,
        params=_parse_params(document.get("params"), resolved),
        timeout_ms=int(defaults.get("timeout_ms", 30_000)),
        quit_on_finish=quit_on_finish,
    )
    _scenario_cache[resolved] = scenario
    return scenario


def resolve_scenario_path(reference: str, *base_dirs: Path) -> Path:
    """Resolve a ``call`` reference against the caller's search directories.

    The caller's own directory is tried first, then (for nested fragments)
    the root scenario's directory; the first existing match wins. Without a
    match the primary candidate is returned so the error names a real path.
    """
    candidate = Path(reference)
    if candidate.suffix != ".toml":
        candidate = candidate.with_name(candidate.name + ".toml")
    if candidate.is_absolute():
        return candidate.resolve()
    resolved = [(base_dir / candidate).resolve() for base_dir in base_dirs]
    for path in resolved:
        if path.exists():
            return path
    return resolved[0] if resolved else candidate.resolve()


def bind_args(scenario: Scenario, args: dict[str, object]) -> dict[str, object]:
    """Merge call arguments over the callee's defaults, rejecting surprises."""
    declared = {param.name for param in scenario.params}
    unknown = set(args) - declared
    if unknown:
        raise ScenarioError(
            f"call to {scenario.path}: unknown argument(s): {', '.join(sorted(unknown))}"
        )
    bound: dict[str, object] = {}
    missing: list[str] = []
    for param in scenario.params:
        if param.name in args:
            bound[param.name] = args[param.name]
        elif param.has_default:
            bound[param.name] = param.default
        else:
            missing.append(param.name)
    if missing:
        raise ScenarioError(
            f"call to {scenario.path}: missing required argument(s): "
            f"{', '.join(sorted(missing))}"
        )
    return bound


_PLACEHOLDER = re.compile(r"\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\}")
_PLACEHOLDER_WHOLE = re.compile(r"^\{\{\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}\}$")


def _placeholder_value(name: str, params: dict[str, object], source: Path) -> object:
    if name not in params:
        declared = ", ".join(sorted(params)) or "<none>"
        raise ScenarioError(
            f"{source}: undefined parameter {name!r} (available: {declared})"
        )
    return params[name]


def substitute(value: object, params: dict[str, object], source: Path) -> object:
    """Replace ``{{ name }}`` placeholders throughout a step tree.

    A string that is exactly one placeholder takes the parameter's own type
    (so numeric/boolean ``expect`` values survive); embedded placeholders are
    stringified.
    """
    if isinstance(value, dict):
        result: dict[object, object] = {}
        for key, item in value.items():
            resolved_key = key
            if isinstance(key, str) and "{{" in key:
                resolved_key = _PLACEHOLDER.sub(
                    lambda match: str(
                        _placeholder_value(match.group(1), params, source)
                    ),
                    key,
                )
            if resolved_key in result:
                raise ScenarioError(
                    f"{source}: parameter substitution produced duplicate key "
                    f"{resolved_key!r}"
                )
            result[resolved_key] = substitute(item, params, source)
        return result
    if isinstance(value, list):
        return [substitute(item, params, source) for item in value]
    if not isinstance(value, str):
        return value
    whole = _PLACEHOLDER_WHOLE.match(value)
    if whole is not None:
        return _placeholder_value(whole.group(1), params, source)
    return _PLACEHOLDER.sub(
        lambda match: str(
            _placeholder_value(match.group(1), params, source)
        ),
        value,
    )


def substitute_steps(
    steps: list[object], params: dict[str, object], source: Path
) -> list[object]:
    return [substitute(step, params, source) for step in steps]


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


def _collect(value: object, components: list[str]) -> list[object]:
    """Collect the values at a dotted path. A ``*`` component fans out over a
    list element; every other component (a dict key or numeric list index)
    selects one value. Raises KeyError when the path is absent or a ``*``
    branch matches nothing."""
    if not components:
        return [value]
    head, rest = components[0], components[1:]
    if isinstance(value, dict):
        if head not in value:
            raise KeyError(head)
        return _collect(value[head], rest)
    if isinstance(value, list):
        if head == "*":
            found: list[object] = []
            for element in value:
                try:
                    found.extend(_collect(element, rest))
                except KeyError:
                    continue
            if not found:
                raise KeyError(head)
            return found
        if head.isdigit() and int(head) < len(value):
            return _collect(value[int(head)], rest)
    raise KeyError(head)


def path_values(observed: object, path: str) -> list[object]:
    """All values at a dotted path; ``*`` fans out over list elements."""
    return _collect(observed, path.split("."))


def _display(values: list[object]) -> str:
    return repr(values[0]) if len(values) == 1 else repr(values)


def conditions_hold(observed: object, expected: dict[str, object]) -> bool:
    for path, wanted in expected.items():
        try:
            values = path_values(observed, path)
        except KeyError:
            return False
        if not any(value == wanted for value in values):
            return False
    return True


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
    if "cargo" in roots:
        result["cargo"] = probe.get("/probe/state?query=cargo")
    if "automation" in roots:
        result["automation"] = probe.get("/probe/automation")
    if "ships" in roots:
        ships = probe.get("/probe/state?query=ships")
        if isinstance(ships, dict) and isinstance(ships.get("ships"), list):
            # Synthetic conveniences for trigger/wait paths: the class display
            # names of the player's attached, non-mission behavior-6 escorts,
            # and their count. Lets a scenario say `ships.escorts.* =
            # "Terrapin"` / `ships.escort_count = 1` without scanning in TOML.
            #
            # This must mirror the cap predicate `Ship_CanPlayerHaveMoreEscorts`
            # (0x00468920): `squad_leader_ship_slot == 0` restricts the count to
            # ships attached to the player. Without it, NPC escorts attached to
            # other ships also match (same behavior code and mission slot) and
            # inflate `escort_count`.
            escorts = [
                ship.get("ship_class")
                for ship in ships["ships"]
                if isinstance(ship, dict)
                and ship.get("ai_behavior_code") == 6
                and ship.get("mission_fleet_slot") == -1
                and ship.get("squad_leader_ship_slot") == 0
                and isinstance(ship.get("ship_class"), str)
            ]
            ships["escorts"] = escorts
            ships["escort_count"] = len(escorts)
        result["ships"] = ships
    unknown = roots - result.keys()
    if unknown:
        raise ScenarioError(f"unknown observation root(s): {', '.join(sorted(unknown))}")
    return result


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
            values = path_values(observed, path)
        except KeyError:
            failures.append(f"{path}: path is absent")
            continue
        if not any(value == wanted for value in values):
            failures.append(f"{path}: expected {wanted!r}, got {_display(values)}")
    for path, pattern in regex_matches.items():
        if not isinstance(pattern, str):
            raise ScenarioError(f"validate_state regex for {path} must be a string")
        try:
            values = path_values(observed, path)
        except KeyError:
            failures.append(f"{path}: path is absent")
            continue
        if not any(re.fullmatch(pattern, str(value)) for value in values):
            failures.append(f"{path}: {_display(values)} does not match /{pattern}/")
    if failures:
        raise ScenarioError("state validation failed: " + "; ".join(failures))


def wait_for(
    probe: Probe, expected: dict[str, object], timeout_ms: int
) -> dict[str, object]:
    deadline = time.monotonic() + timeout_ms / 1000
    last: dict[str, object] = {}
    while time.monotonic() < deadline:
        last = observations(probe, expected)
        if conditions_hold(last, expected):
            return last
        automation = last.get("automation")
        if isinstance(automation, dict) and automation.get("phase") == "failed":
            raise ScenarioError(f"automation failed: {automation.get('detail', '')}")
        time.sleep(0.05)
    raise ScenarioError(
        f"timed out waiting for {expected!r}; last observation: {last!r}"
    )


def wait_for_any(
    probe: Probe, alternatives: list[dict[str, object]], timeout_ms: int
) -> dict[str, object]:
    """Wait until any one of the alternatives fully holds. Lets a route
    branch on which modal the game opened next (e.g. an optional overwrite
    confirmation) without racing a one-shot ``trigger`` check."""
    # Fetch the union of every root any alternative needs, once per poll.
    merged: dict[str, object] = {}
    for expected in alternatives:
        for path in expected:
            merged.setdefault(path, None)
    deadline = time.monotonic() + timeout_ms / 1000
    last: dict[str, object] = {}
    while time.monotonic() < deadline:
        last = observations(probe, merged)
        for expected in alternatives:
            if conditions_hold(last, expected):
                return last
        automation = last.get("automation")
        if isinstance(automation, dict) and automation.get("phase") == "failed":
            raise ScenarioError(f"automation failed: {automation.get('detail', '')}")
        time.sleep(0.05)
    raise ScenarioError(
        f"timed out waiting for any of {alternatives!r}; "
        f"last observation: {last!r}"
    )


def _item_target(ui: object, where: dict[str, object]) -> str | None:
    """First /probe/ui `items` entry whose fields all equal `where`, by name."""
    items = ui.get("items") if isinstance(ui, dict) else None
    if not isinstance(items, list):
        return None
    for item in items:
        if not isinstance(item, dict):
            continue
        if all(item.get(key) == wanted for key, wanted in where.items()):
            name = item.get("name")
            if isinstance(name, str) and name:
                return name
    return None


def click_when_available(
    probe: Probe,
    element: object,
    where: object,
    timeout_ms: int,
) -> None:
    if where is not None:
        if not isinstance(where, dict) or not where:
            raise ScenarioError("click where must be a non-empty table")
    elif not isinstance(element, str) or not element:
        raise ScenarioError("click requires a non-empty element or a where table")
    deadline = time.monotonic() + timeout_ms / 1000
    while time.monotonic() < deadline:
        ui = probe.get("/probe/ui")
        target = None
        if where is not None:
            target = _item_target(ui, where)
        elif isinstance(ui, dict) and element in ui.get("rects", {}):
            target = element
        if target is not None:
            try:
                probe.post("/probe/click", {"element": target})
                return
            except urllib.error.HTTPError as error:
                if error.code != 409:
                    raise
        time.sleep(0.02)
    wanted = where if where is not None else element
    raise ScenarioError(f"timed out waiting to click UI element {wanted!r}")


def decline_missions(probe: Probe, timeout_ms: int, settle_ms: int) -> int:
    """Decline pending mission offers and close their refuse readers.

    Clicks `decline` while the active modal is `mission_offer`, then `done`
    while it is a `text_reader` (the offer-decline follow-up). Returns once no
    such modal has appeared for `settle_ms`; the initial quiet window is what
    lets a just-opened bar's offer surface. Returns how many modals it closed.
    """
    deadline = time.monotonic() + timeout_ms / 1000
    settle = max(0.0, settle_ms / 1000)
    quiet_since = time.monotonic()
    dismissed = 0
    while time.monotonic() < deadline:
        ui = probe.get("/probe/ui")
        window = ui.get("window") if isinstance(ui, dict) else None
        rects = ui.get("rects", {}) if isinstance(ui, dict) else {}
        element = None
        if window == "mission_offer" and "decline" in rects:
            element = "decline"
        elif window == "text_reader" and "done" in rects:
            element = "done"
        if element is not None:
            probe.post("/probe/click", {"element": element})
            dismissed += 1
            quiet_since = time.monotonic()
            time.sleep(0.1)
            continue
        if time.monotonic() - quiet_since >= settle:
            return dismissed
        time.sleep(0.05)
    ui = probe.get("/probe/ui")
    window = ui.get("window") if isinstance(ui, dict) else None
    if window in ("mission_offer", "text_reader"):
        raise ScenarioError(
            f"decline_mission did not settle (still on {window!r} after "
            f"dismissing {dismissed} modal(s))"
        )
    return dismissed


def capture_diagnostics(probe: Probe, directory: Path, step_label: str) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    snapshot: dict[str, object] = {"failed_step": step_label}
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


def run_steps(
    context: RunContext,
    steps: list[object],
    default_timeout_ms: int,
    prefix: str = "",
) -> None:
    for index, step in enumerate(steps, 1):
        label = f"{prefix}.{index}" if prefix else str(index)
        try:
            if not isinstance(step, dict):
                raise ScenarioError(f"step {label} is not a table")
            print(f"[{label}] {step.get('action', '<missing>')}", flush=True)
            run_step(context, step, default_timeout_ms, label)
        except StepFailure:
            raise
        except Exception as error:
            capture_diagnostics(context.probe, context.artifacts, label)
            raise StepFailure(label, error) from error


def run_step(
    context: RunContext,
    step: dict[str, object],
    default_timeout_ms: int,
    step_label: str,
) -> None:
    probe = context.probe
    artifacts = context.artifacts
    action = step.get("action")
    common = {"action", "trigger", "trigger_not"}
    allowed = {
        "wait": {"expect", "expect_any", "timeout_ms"},
        "validate_state": {"expect", "matches"},
        "click": {"element", "where", "timeout_ms"},
        "key": {"key"},
        "hold": {"keys", "down"},
        "command": {"cmd", "target", "ship_id", "allow_missing", "commodity", "side", "tons", "max", "timeout_ms", "enabled", "speed_multiplier", "suppress_audio"},
        "screenshot": {"name"},
        "repeat": {"count", "steps", "until"},
        "decline_mission": {"timeout_ms", "settle_ms"},
        "call": {"scenario", "args"},
        "quit": set(),
    }
    if action not in allowed:
        raise ScenarioError(f"unknown action {action!r}")
    unknown = set(step) - (allowed[str(action)] | common)
    if unknown:
        raise ScenarioError(f"unknown {action} field(s): {', '.join(sorted(unknown))}")

    # Optional conditions evaluated before the action. `trigger` must hold and
    # `trigger_not` must not; either failing skips the step. Conditions accept
    # the same dotted paths as expect, including `*` over list elements.
    trigger = step.get("trigger")
    trigger_not = step.get("trigger_not")
    if trigger is not None or trigger_not is not None:
        conditions: dict[str, object] = {}
        if trigger is not None:
            if not isinstance(trigger, dict) or not trigger:
                raise ScenarioError("trigger must be a non-empty table")
            conditions.update(trigger)
        if trigger_not is not None:
            if not isinstance(trigger_not, dict) or not trigger_not:
                raise ScenarioError("trigger_not must be a non-empty table")
            conditions.update(trigger_not)
        observed = observations(probe, conditions)
        if trigger is not None and not conditions_hold(observed, trigger):
            print(f"[{step_label}] skip {action}: trigger not met", flush=True)
            return
        if trigger_not is not None and conditions_hold(observed, trigger_not):
            print(f"[{step_label}] skip {action}: trigger_not met", flush=True)
            return

    if action == "wait":
        expected = step.get("expect")
        alternatives = step.get("expect_any")
        if expected is not None and alternatives is not None:
            raise ScenarioError(
                "wait accepts either expect or expect_any, not both"
            )
        timeout_ms = int(step.get("timeout_ms", default_timeout_ms))
        if alternatives is not None:
            if (
                not isinstance(alternatives, list)
                or not alternatives
                or not all(
                    isinstance(item, dict) and item for item in alternatives
                )
            ):
                raise ScenarioError(
                    "wait expect_any must be a non-empty array of non-empty tables"
                )
            wait_for_any(probe, alternatives, timeout_ms)
        else:
            if not isinstance(expected, dict) or not expected:
                raise ScenarioError("wait requires a non-empty expect table")
            wait_for(probe, expected, timeout_ms)
    elif action == "validate_state":
        expected = step.get("expect", {})
        regex_matches = step.get("matches", {})
        if not isinstance(expected, dict) or not isinstance(regex_matches, dict):
            raise ScenarioError("validate_state expect and matches must be tables")
        validate_state(probe, expected, regex_matches)
    elif action == "click":
        element = step.get("element")
        where = step.get("where")
        if element is not None and where is not None:
            raise ScenarioError("click accepts either element or where, not both")
        click_when_available(
            probe,
            element,
            where,
            int(step.get("timeout_ms", default_timeout_ms)),
        )
    elif action == "key":
        probe.post("/probe/key", {"key": step["key"]})
    elif action == "hold":
        keys = step.get("keys")
        down = step.get("down")
        if (
            not isinstance(keys, list)
            or not keys
            or not all(isinstance(key, str) for key in keys)
        ):
            raise ScenarioError("hold requires a non-empty keys array")
        if not isinstance(down, bool):
            raise ScenarioError("hold requires down = true or false")
        probe.post("/probe/hold", {"keys": keys, "down": down})
    elif action == "command":
        probe.post(
            "/probe/command",
            {key: value for key, value in step.items() if key not in common},
        )
    elif action == "screenshot":
        name = str(step.get("name", "screenshot"))
        if Path(name).name != name:
            raise ScenarioError("screenshot name must not contain a directory")
        artifacts.mkdir(parents=True, exist_ok=True)
        (artifacts / f"{name}.bmp").write_bytes(probe.screenshot())
    elif action == "decline_mission":
        dismissed = decline_missions(
            probe,
            int(step.get("timeout_ms", default_timeout_ms)),
            int(step.get("settle_ms", 300)),
        )
        print(
            f"[{step_label}] decline_mission closed {dismissed} modal(s)",
            flush=True,
        )
    elif action == "repeat":
        count = step.get("count")
        nested = step.get("steps")
        until = step.get("until")
        if isinstance(count, bool) or not isinstance(count, int) or count < 0:
            raise ScenarioError("repeat requires a non-negative integer count")
        if not isinstance(nested, list) or not nested:
            raise ScenarioError("repeat requires a non-empty steps array")
        if until is not None and (not isinstance(until, dict) or not until):
            raise ScenarioError("repeat until must be a non-empty table")
        for iteration in range(1, count + 1):
            print(f"[{step_label}] repeat {iteration}/{count}", flush=True)
            run_steps(
                context,
                nested,
                default_timeout_ms,
                f"{step_label}.{iteration}",
            )
            if until is not None:
                observed = observations(probe, until)
                if conditions_hold(observed, until):
                    print(
                        f"[{step_label}] repeat stopped: until met "
                        f"after {iteration}/{count}",
                        flush=True,
                    )
                    break
    elif action == "call":
        reference = step.get("scenario")
        args = step.get("args", {})
        if not isinstance(reference, str) or not reference:
            raise ScenarioError("call requires a non-empty scenario")
        if not isinstance(args, dict):
            raise ScenarioError("call args must be a table")
        search_dirs = [context.base_dir]
        if context.stack:
            search_dirs.append(context.stack[0].parent)
        target = resolve_scenario_path(reference, *search_dirs)
        if target in context.stack:
            chain = " -> ".join(path.name for path in (*context.stack, target))
            raise ScenarioError(f"recursive scenario call: {chain}")
        if len(context.stack) >= _MAX_CALL_DEPTH:
            raise ScenarioError(
                f"call depth exceeded {_MAX_CALL_DEPTH} at {target}"
            )
        called = load_scenario(target)
        bound = bind_args(called, args)
        steps = substitute_steps(called.steps, bound, called.path)
        print(
            f"[{step_label}] call {called.name!r} ({called.path})", flush=True
        )
        run_steps(
            RunContext(
                probe=probe,
                artifacts=artifacts,
                base_dir=called.path.parent,
                stack=(*context.stack, target),
            ),
            steps,
            called.timeout_ms,
            f"{step_label}->{called.name}",
        )
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
        scenario = load_scenario(args.scenario)
        quit_on_finish = scenario.quit_on_finish
        context = RunContext(
            probe=probe,
            artifacts=args.artifacts / scenario.name,
            base_dir=scenario.path.parent,
            stack=(scenario.path,),
        )
        root_params = bind_args(scenario, {})
        root_steps = substitute_steps(scenario.steps, root_params, scenario.path)
        run_steps(context, root_steps, scenario.timeout_ms)
        print(f"scenario {scenario.name!r} passed")
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
