"""Verified resource decoding and lazy typed provider contracts."""
import unittest
import tempfile
import shutil
from pathlib import Path
from unittest.mock import patch

from test_scenario import REPO, HAS_DATA
import scenario_providers as stock
from ghidra import Field, parse_layout
from providers import validate_record
from model import Candidate, Ledger, Spec
from providers import Provider
from store import StateDB
from world import World
import bridge


class GovernmentProviderTests(unittest.TestCase):
    def test_selection_removal_and_recreation_preserve_other_generators(self):
        definitions = {"scenario:government:128": Provider(
            "scenario:government:128", "parent_sampler", "g_government_defs[0]",
            [Candidate("resource:128", "record", {"flags_primary": 7}, prior_evidence="observed")], alternative=True)}
        other = Provider("global_sampler:g_cache", "global_sampler", "g_cache",
                         [Candidate("cold", "expr", "-1.0", prior_evidence="review")])
        with tempfile.TemporaryDirectory() as tmp:
            state = Path(tmp) / "analysis"
            for fresh in range(2):
                db = StateDB(state)
                try:
                    world = World(db)
                    world.providers[other.id] = other
                    world.save()
                    store = db.store("0x0040fd20")
                    store.save_spec(Spec("0x0040fd20", "F", "bool F(void)", "unused.exe"))
                    store.save_ledger(Ledger())
                    db.commit("fixture", cause="test")
                    stock.select_providers(db, definitions)
                    self.assertIn(other.id, World(db).providers)
                    self.assertIn("scenario:government:128", store.load_ledger().providers)
                    count = len(db.log())
                    stock.select_providers(db, definitions)
                    self.assertEqual(len(db.log()), count)  # unchanged selection has no new event
                    stock.select_providers(db, {})
                    self.assertEqual(set(World(db).providers), {other.id})
                    self.assertFalse(store.load_ledger().providers)
                finally:
                    db.close()
                if not fresh:
                    shutil.rmtree(state)  # source definitions survive deleting all generated state

    def test_signed_classes_and_unsigned_flags(self):
        payload = bytearray(0x30)
        payload[2:4] = bytes.fromhex("e2b0")
        payload[0x18:0x20] = bytes.fromhex("ffff000000647fff")
        record = stock.government_record(payload)
        self.assertEqual(record["flags_primary"], 0xe2b0)
        self.assertEqual([record[f"class_{i}"] for i in range(1, 5)], [-1, 0, 100, 32767])
        self.assertNotIn("is_present", record)
        with self.assertRaises(ValueError):
            stock.government_record(payload[:0x2f])

    def test_index_identity_holes_and_archive_precedence(self):
        with patch.object(stock, "records", return_value=[(130, bytes(0x30)), (130, b"\0\0\0\7" + bytes(0x2c))]):
            providers = stock.government_providers(REPO)
        self.assertEqual(list(providers), ["scenario:government:130"])
        provider = providers["scenario:government:130"]
        self.assertEqual(provider.selector, "g_government_defs[2]")
        self.assertEqual(provider.candidates[0].value["flags_primary"], 7)
        for records in ([], [(127, bytes(0x30))], [(384, bytes(0x30))]):
            with patch.object(stock, "records", return_value=records), self.assertRaises(ValueError):
                stock.government_providers(REPO)

    @unittest.skipUnless(HAS_DATA, "stock archives not present")
    def test_stock_records_fit_verified_layout(self):
        # Only the verified fields are needed; a changed name or width must fail.
        layout = parse_layout("Type: GovtDef\nSize: 136\n0x20 2 ushort flags_primary\n" + "\n".join(
            f"0x{offset + i * 2:x} 2 short {stem}_{i + 1}"
            for stem, offset in (("class", 0x26), ("ally_class", 0x2e), ("enemy_class", 0x36))
            for i in range(4)))
        class Types:
            def layout(self, name):
                return layout if name == "GovtDef" else None
        providers = stock.government_providers(REPO)
        self.assertEqual(len(providers), 68)
        for provider in providers.values():
            validate_record(Types(), "GovtDef", provider.candidates[0].value)
        federation = providers["scenario:government:128"].candidates[0].value
        self.assertEqual(federation["flags_primary"], 0xe2b0)
        self.assertEqual([federation[f"enemy_class_{i}"] for i in range(1, 5)], [2, 10, 16, 9])


class _Layout:
    def __init__(self, fields):
        self.fields = tuple(fields)


class _Types:
    """Minimal Ghidra stand-in exposing only scalar layouts."""

    def __init__(self, layouts):
        self._layouts = layouts

    def layout(self, name):
        return self._layouts.get(name)


def _widget_family():
    return stock.Family("widgets", "g_widget_defs", b"w\x00d\x00", "WidgetDef", "DecodeWidget", 0x100)


class DirectFieldTests(unittest.TestCase):
    def test_only_clean_same_or_widening_scalar_copies_survive(self):
        layout = _Layout((Field(0, 2, "short", "clean"), Field(2, 2, "short", "derived"),
                          Field(4, 4, "float", "floating"), Field(8, 2, "short", "renamed"),
                          Field(10, 4, "int", "widened"), Field(14, 2, "short", "ambiguous")))
        source = '''
        void Decode(bytes) {
          s.clean = ReadBeI16(bytes, 0x00);
          s.derived = ReadBeI16(bytes, 0x02);
          if (s.derived < 0) { s.derived = 0; }
          s.floating = ReadBeI16(bytes, 0x04);
          s.local_name = ReadBeI16(bytes, 0x06);
          s.widened = ReadBeI16(bytes, 0x08);
          s.ambiguous = ReadBeI16(bytes, 0x0a);
        }
        '''
        # `ambiguous` matches a layout field and is also renamed onto another one -> rejected.
        aliases = {"local_name": "renamed", "ambiguous": "renamed"}
        fields = stock.direct_fields(source, layout, aliases)
        self.assertEqual(fields, {"clean": (0x00, 2, True), "renamed": (0x06, 2, True),
                                  "widened": (0x08, 2, True)})

    def test_bridge_rename_aliases_exclude_rebased_rules(self):
        aliases = stock._bridge_aliases("ShipClassDef")
        self.assertEqual(aliases["base_shield"], "base_shield_points")
        # SystemDef's nav_stellar_ids rule carries a rebasing `add` and is not usable as a memory value.
        self.assertNotIn("nav_defs", stock._bridge_aliases("SystemDef"))


class FamilyProviderTests(unittest.TestCase):
    def test_partial_records_use_indexed_selector_and_omit_derived_fields(self):
        layout = _Layout((Field(0, 2, "short", "clean"), Field(2, 2, "short", "derived"),
                          Field(4, 4, "int", "widened")))
        source = '''
        void DecodeWidget(bytes) {
          s.clean = ReadBeI16(bytes, 0x00);
          s.derived = ReadBeI16(bytes, 0x02);
          s.derived = s.derived + 1;
          s.widened = ReadBeI16(bytes, 0x04);
        }
        '''
        payload = bytes.fromhex("0005ffff0007")
        with patch.object(stock, "decoder_source", return_value=source), \
                patch.object(stock, "records", return_value=[(128, payload), (200, payload)]):
            providers = stock.family_providers(REPO, _Types({"WidgetDef": layout}), _widget_family())
        self.assertEqual(list(providers), ["scenario:widgets:128", "scenario:widgets:200"])
        provider = providers["scenario:widgets:128"]
        self.assertEqual(provider.selector, "g_widget_defs[0]")
        self.assertTrue(provider.alternative)
        self.assertEqual(provider.candidates[0].value, {"clean": 5, "widened": 7})
        self.assertEqual(provider.candidates[0].prior_evidence, "observed")

    def test_resource_id_outside_the_runtime_table_is_rejected(self):
        layout = _Layout((Field(0, 2, "short", "clean"),))
        source = "void DecodeWidget(bytes) { s.clean = ReadBeI16(bytes, 0x00); }"
        with patch.object(stock, "decoder_source", return_value=source), \
                patch.object(stock, "records", return_value=[(128 + 0x100, bytes(2))]):
            with self.assertRaises(ValueError):
                stock.family_providers(REPO, _Types({"WidgetDef": layout}), _widget_family())

    @unittest.skipUnless(HAS_DATA, "stock archives not present")
    def test_stock_fleets_decode_to_the_loader_domain(self):
        family = stock.Family("fleets", "g_random_encounter_fleet_defs", b"fl\x91t",
                              "RandomEncounterFleetDef", "DecodeFleet", 0x100)
        types = _Types({"RandomEncounterFleetDef": parse_layout(
            "Type: RandomEncounterFleetDef\nSize: 292\n"
            "0x0 2 short lead_ship_class_id\n0x2 2 short government_id\n"
            "0x4 8 short[4] escort_ship_class_ids\n0xC 8 short[4] escort_min_count\n"
            "0x14 8 short[4] escort_max_count\n0x1C 2 short spawn_system_filter\n"
            "0x1E 2 short arrival_message_id\n0x120 2 ushort carry_cargo_flags")})
        providers = stock.family_providers(REPO, types, family)
        self.assertEqual(len(providers), 128)
        federation = providers["scenario:fleets:128"].candidates[0].value
        self.assertEqual(federation, {"spawn_system_filter": 10000, "arrival_message_id": 0})


class ProviderBridgeMappingTests(unittest.TestCase):
    def test_generated_slot_keys_translate_to_port_records(self):
        rules = bridge.Rules.load(REPO / "tools/microexec_evnova/rules.json")
        rules.bind(bridge.HostInfo.parse(
            "root game::GameState\nrecord game::GameState\n"
            "field scenario record:game::Scenario\nrecord game::Scenario\n"
            "field weapons vector:record:game::WeaponDef\nrecord game::WeaponDef\n"
            "field flags_primary ushort\nfield mass_damage short\n"
            "field dudes vector:record:game::DudeDef\nrecord game::DudeDef\nfield ai_type short\n"))
        for key, index, port in (("g_weapon_defs[*].flags_primary", 5, "scenario.weapons[5].flags"),
                                 ("g_weapon_defs[*].mass_damage", 5, "scenario.weapons[5].mass_damage"),
                                 ("g_dude_defs[*].ai_type", 3, "scenario.dudes[3].ai_type")):
            translated = rules.translate(key, index, 1, {})
            self.assertIsNotNone(translated, key)
            self.assertEqual(translated.port, port)


class NpcWeaponMappingTests(unittest.TestCase):
    def test_reload_and_effective_range_are_distinct_from_particle_speed(self):
        rules = bridge.Rules.load(REPO / "tools/microexec_evnova/rules.json")
        for field, port in (("reload_interval_ticks", "reload_ticks"),
                            ("effective_range_px", "range_scalar"),
                            ("+0x5c", "range_scalar"),
                            ("field_0x5c", "range_scalar"),
                            ("range_scalar", "trail_particle_speed_variants[0]")):
            translated = rules.translate(f"g_weapon_defs[*].{field}", 3, 42, {})
            self.assertEqual((translated.port, translated.value),
                             (f"scenario.weapons[3].{port}", 42))
        rating = rules.translate("g_player_combat_rating_points", 0, -10, {})
        self.assertEqual((rating.port, rating.value), ("player_combat_rating_points", -10))

    def test_trig_tables_and_audio_handles_are_skipped(self):
        rules = bridge.Rules.load(REPO / "tools/microexec_evnova/rules.json")
        for key in ("g_angle_sine_table[*]", "g_angle_cosine_table[*]",
                    "g_gameplay_sound_handle_table[*]"):
            self.assertIsInstance(rules.translate(key, 0, 1, {}), str)


class RankPrivilegeMappingTests(unittest.TestCase):
    def test_both_array_bytes_map_to_named_fields_and_reverse(self):
        rules = bridge.Rules.load(REPO / "tools/microexec_evnova/rules.json")
        rules.bind(bridge.HostInfo.parse("root game::GameState\nrecord game::GameState\n"
            "field scenario record:game::Scenario\nrecord game::Scenario\n"
            "field governments vector:record:game::Government\nrecord game::Government\n"
            "field rank_privileges record:game::Privileges\nrecord game::Privileges\n"
            "field no_auto_attack bool\nfield always_land bool\n"))
        for index, member in enumerate(("no_auto_attack", "always_land")):
            path = f"scenario.governments[7].rank_privileges.{member}"
            for value in (0, 1):
                translated = rules.translate("g_government_defs[*].rank_privileges[*]", 7, value, {},
                                             path=f"g_government_defs[7].rank_privileges[{index}]")
                self.assertEqual((translated.port, translated.value), (path, value))
            self.assertEqual(rules.reverse(path), (f"g_government_defs[*].rank_privileges[{index}]", 7))
        self.assertIsNone(rules.translate("g_government_defs[*].rank_privileges[*]", 7, 1, {},
                                         path="g_government_defs[7].rank_privileges[2]"))


if __name__ == "__main__":
    unittest.main()
