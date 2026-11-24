"""Opt-in original-executable tests; temporary state, no model calls or game probe.

EVN_MICROEXEC_LIVE=1 tools/.venv/bin/python -m unittest discover -s tools/microexec_evnova -p test_live.py -v
"""
import copy
from dataclasses import replace
import os
import tempfile
import unittest
from pathlib import Path

from test_scenario import PROJECT
from scenario_providers import government_providers
import bridge
import cases
from constraints import Constraint
from emulator import Context, Run
from ghidra import Ghidra, is_pointer
from judges import PriorJudge
from model import Case, Ledger, Spec
from pe_image import load_pe
from providers import proposal_provider
from store import StateDB


@unittest.skipUnless(os.environ.get("EVN_MICROEXEC_LIVE") == "1", "opt-in live Ghidra/executable test")
class LiveAdapterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="evnova-microexec-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.db = StateDB(self.root)
        self.addCleanup(self.db.close)
        self.g = Ghidra(self.root / "cache", base=PROJECT.ghidra, refresh=True)
        self.image = load_pe(PROJECT.image)
        self.host = bridge.find_host(PROJECT.port)
        self.assertIsNotNone(self.host, "build evnova_microexec_host first")
        self.host_info = bridge.describe_host(self.host)
        self.rules = bridge.Rules.load(PROJECT.port.rules).bind(self.host_info, self.g)
        # These cases deliberately select stock records; option mixing is tested separately.
        self.providers = {key: replace(value, alternative=False, revision="")
                          for key, value in government_providers(PROJECT.root).items()}
        self.run_id = 0

    def context(self, addr):
        info = self.g.function(int(addr, 16))
        store = self.db.store(addr)
        store.save_spec(Spec(addr, info.name, info.signature, str(PROJECT.image), wall_time_budget=2.0))
        store.save_ledger(Ledger(providers=copy.deepcopy(self.providers)))
        return Context(store, self.g, self.image,
                       PriorJudge(self.root / "cache", enabled=False, project=PROJECT),
                       PROJECT, log=lambda _: None)

    def run_case(self, ctx, case):
        self.run_id += 1
        result = Run(ctx, self.run_id, case.seed, case.mode, case).execute()
        self.assertEqual(result["outcome"], "returned", result.get("detail"))
        return result

    def compare(self, ctx, runs):
        target = bridge.port_target(self.rules, ctx.spec.target, ctx.spec.signature, self.host_info)
        result = bridge.compare(runs, self.rules, ctx.spec, ctx.ledger, self.host, self.g, target)
        for row in result["runs"]:
            self.assertEqual(row["status"], "match", row)
            for key in ("read_unset_mapped", "read_unset_unmapped", "unmapped_writes", "port_only_unmapped"):
                self.assertFalse(row.get(key), row)

    def test_price_boundaries_and_float_arguments_match_native_port(self):
        ctx = self.context("0x0049d640")
        runs = [self.run_case(ctx, Case.make("baseline", {
            "arg.param_1": price, "arg.param_2": scale,
            "arg.param_3": item_tech, "arg.param_4": stellar_tech}))
            for price in (-1, 0, 1, 99, 100, 101, 10000, 10001, 100000, 100001)
            for scale, item_tech, stellar_tech in ((1.0, 0, 0), (0.5, 0, 5), (1.25, 5, 0))]
        self.compare(ctx, runs)

    def test_optional_stock_and_synthetic_sources_both_execute_and_match(self):
        ctx = self.context("0x0046bc90")
        ctx.ledger.providers = government_providers(PROJECT.root)
        leaf = proposal_provider("g_government_defs[*].flags_primary", {
            "id": "experiment:flags", "scope": "global",
            "operation": {"op": "exact", "value": 0x800}})
        ctx.ledger.providers[leaf.id] = leaf
        # This test varies generator sources, with a materialized government table.
        # Unconstrained pointer fuzzing also offers a null table, which would fault.
        self.run_case(ctx, Case.make("baseline", {"arg.govt_a": 0, "arg.govt_b": 1}))
        for assumption in ctx.ledger.assumptions.values():
            if is_pointer(assumption.slot_type):
                assumption.candidates = [c for c in assumption.candidates if c.kind == "fresh"]
        runs = [self.run_case(ctx, Case.make("fuzz", {"arg.govt_a": 0, "arg.govt_b": 1}, seed=seed, mode="sample"))
                for seed in range(24)]
        sources = {t.get("provider") for run in runs for t in run["touched"]
                   if t["key"].endswith(".flags_primary")}
        self.assertIn(leaf.id, sources)
        self.assertTrue(any(source and source.startswith("scenario:government:") for source in sources))
        self.compare(ctx, runs)

    def test_local_leaf_composes_with_stock_record(self):
        ctx = self.context("0x0046bc90")
        records = [p.candidates[0].value for p in self.providers.values()]
        # Pick a genuinely allied, distinct stock pair to exercise the flag guard.
        pair = next((a, b) for a, ra in enumerate(records) for b, rb in enumerate(records)
                    if a != b and not ((ra["flags_primary"] | rb["flags_primary"]) & 0x800)
                    and any(ra[f"class_{i}"] >= 0 and ra[f"class_{i}"] == rb[f"ally_class_{j}"]
                            for i in range(1, 5) for j in range(1, 5)))
        base = Case.make("baseline", dict(zip(("arg.govt_a", "arg.govt_b"), pair)))
        original = self.run_case(ctx, base)
        self.assertEqual(original["return"]["eax"] & 0xff, 1)
        leaf = proposal_provider("g_government_defs[*].flags_primary", {
            "id": "experiment:derelict", "scope": "local",
            "operation": {"op": "exact", "value": 0x800}})
        leaf.candidates[0].prior_evidence = "observed"
        ctx.ledger.providers[leaf.id] = leaf
        # Fresh construction gets the local override; a recorded base stays stable.
        overridden = self.run_case(ctx, Case.make("baseline", base.overrides, seed=1))
        self.assertEqual(overridden["return"]["eax"] & 0xff, 0)
        flags = [t for t in overridden["touched"] if t["key"].endswith(".flags_primary")]
        self.assertTrue(flags)
        self.assertTrue(all(t["provider"] == leaf.id for t in flags))
        self.compare(ctx, [overridden])

    def test_stock_relations_and_boundary_paths_match_native_port(self):
        ids = [int(p.selector.split("[")[1][:-1]) for p in self.providers.values()]
        pairs = [(i, j) for i in ids[:8] for j in ids[:8]]
        pairs += [(ids[0], i) for i in ids[8:]]
        pairs += [(-1, -1), (-1, 0), (0, -1), (256, 0), (0, 256), (256, 256)]
        for addr in ("0x0046bc90", "0x0046bff0"):
            ctx = self.context(addr)
            runs = [self.run_case(ctx, Case.make("baseline", {"arg.govt_a": a, "arg.govt_b": b}))
                    for a, b in pairs]
            for result in runs:
                for touch in result["touched"]:
                    if touch["key"].startswith("g_government_defs[*]."):
                        self.assertTrue(touch["provider"].startswith("scenario:government:"), touch)
            self.compare(ctx, runs)

    def test_persisted_replay_mutation_and_fixed_construction(self):
        ctx = self.context("0x0046bc90")
        base = Case.make("baseline", {"arg.govt_a": 0, "arg.govt_b": 1})
        first = self.run_case(ctx, base)
        self.assertTrue(base.roots)
        saved = copy.deepcopy(base.inputs)
        ctx.store.save_cases({base.id: base})
        # Changed scenario definitions must not rewrite concrete recorded inputs.
        ctx.ledger.providers = {}
        base = ctx.store.load_cases()[base.id]
        replay = self.run_case(ctx, base)
        self.assertEqual(base.inputs, saved)
        self.assertEqual(replay["return"], first["return"])
        self.compare(ctx, [replay])

        ctx.ledger.providers = copy.deepcopy(self.providers)
        ctx.ledger.constraints["next-govt"] = Constraint(
            "next-govt", "arg.govt_b", "construction",
            operation={"op": "derive", "source": "arg.govt_a", "offset": 1})
        coherent = Case.make("baseline", {"arg.govt_a": 0})
        self.run_case(ctx, coherent)
        changed = cases.derive(coherent, "grid", {"arg.govt_a": 2})
        mutated = self.run_case(ctx, changed)
        b = next(t for t in changed.inputs if t["key"] == "arg.govt_b")
        self.assertEqual(int.from_bytes(bytes.fromhex(b["bytes"]), "little"), 3)
        self.assertEqual(b["construction"][0]["dependencies"], ["arg.govt_a"])
        self.assertEqual(changed.parent, coherent.id)
        self.compare(ctx, [mutated])

        erased = self.run_case(ctx, cases.derive(coherent, "grid", {"arg.govt_b": 7}))
        self.assertEqual(erased["erased_mutations"][0]["proposed"], 7)
        fixed = cases.derive(coherent, "frontier", {"arg.govt_b": 7})
        conflict = Run(ctx, 999, 0, "argmax", fixed).execute()
        self.assertEqual(conflict["outcome"], "construction_error", conflict.get("detail"))
        self.assertNotIn("arg.govt_b", [t["key"] for t in fixed.inputs])


if __name__ == "__main__":
    unittest.main()
