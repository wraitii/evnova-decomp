"""EV Nova scenario plugin: field distributions measured from the shipped Nova Data archives.

Run: python3 -m unittest discover -s tools/microexec_evnova
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO.parent / "microexec" / "src"))

import project as projects  # noqa: E402

PROJECT = projects.load(HERE / projects.FILE_NAME)
PLUGIN = PROJECT.plugins[0]
HAS_DATA = any((REPO / "EV Nova" / "Nova Files").glob("Nova Data *.rez"))


class ProjectTests(unittest.TestCase):
    def test_profile_is_discovered_from_repo_and_nested_directory(self):
        self.assertEqual(projects.discover(REPO), HERE / projects.FILE_NAME)
        self.assertEqual(projects.discover(REPO / "src/game"), HERE / projects.FILE_NAME)

    def test_evnova_project_renders(self):
        from bounds import build_prompt
        import candidates
        from judges import review_prompt, value_prior_definition
        from model import Assumption

        self.assertEqual([p.name for p in PROJECT.plugins], ["scenario"])
        slots = [Assumption(key, "short", 2, "memory", True,
                            candidates.candidates_for("short", 2, []))
                 for key in ("g_a", "g_b", "g_c")]
        context = {"g_b": {"revision_reason": "blamed"},
                   "g_c": {"revision_reason": "cov", "coverage_frontier": {"id": "e"}}}
        bounds = build_prompt({"F": "int F() { return 1; }"}, slots, context, project=PROJECT,
                              sections=PROJECT.sections([s.key for s in slots]))
        review = review_prompt(Path("b.md"), PROJECT)
        jev = str(value_prior_definition(PROJECT))
        for rendered in (bounds, review, jev):
            self.assertNotIn("{{", rendered)
        for needle in ("scenario_stats", "port_source", "reference_search",
                       "## Scenario families", "## Our C++ port"):
            self.assertIn(needle, bounds)
        self.assertIn("mid-game EV Nova session", jev)


@unittest.skipUnless(HAS_DATA, "shipped Nova Data archives not present")
class ScenarioTests(unittest.TestCase):
    def test_outfit_cost_resolves_through_the_port_decoder(self):
        hint = PLUGIN.slot_hint("g_outfit_defs[*].cost")
        self.assertEqual((hint["payload_offset"], hint["width_bytes"], hint["signed"]), ("0xe", 4, True))
        self.assertGreater(hint["records"], 200)
        self.assertIsNone(PLUGIN.slot_hint("g_outfit_defs[*].no_such_field"))
        self.assertIsNone(PLUGIN.slot_hint("arg.ship[*].cost"))

    def test_evidence_kinds(self):
        kinds = PLUGIN.evidence_kinds()
        stats = kinds["scenario_stats"].handler({"type": "6f9f7466", "offset": 14, "width": 4, "signed": True})
        self.assertIn('"records"', stats)
        self.assertIn("ReadBeI32", kinds["scenario_loader"].handler({"family": "outfits"}))
        with self.assertRaises(ValueError):
            kinds["scenario_loader"].handler({"family": "nope"})





@unittest.skipUnless(HAS_DATA, "shipped Nova Data archives not present")
class CargoHintTests(unittest.TestCase):
    def test_holds_hint_accounts_for_loader_normalization(self):
        hint = PLUGIN.slot_hint('g_ship_class_defs[*].cargo_holds')
        self.assertEqual((hint['payload_offset'], hint['width_bytes'], hint['signed']), ('0x0', 2, True))
        self.assertEqual(hint['records'], 288)
        self.assertGreaterEqual(hint['min'], 0)
        self.assertIn('normalization', hint)
        self.assertIn('ReadBeI16', hint['port_line'])


if __name__ == "__main__":
    unittest.main()
