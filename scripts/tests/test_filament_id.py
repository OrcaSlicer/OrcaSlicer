#!/usr/bin/env python3
"""Tests for scripts/assign_filament_ids.py (stdlib unittest, no external deps).

Run from the repo root:  python -m unittest discover -s scripts/tests -v
"""

import contextlib
import io
import json
import os
import re
import shutil
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import assign_filament_ids as afi  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REAL_PROFILES = os.path.join(REPO_ROOT, "resources", "profiles")

OFL = "OrcaFilamentLibrary"


def load_json_file(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# helpers: synthetic profile trees
# ---------------------------------------------------------------------------

def preset(name, filament_id=None, inherits=None, instantiation=True,
           compatible_printers=None, filament_vendor=None, filament_type=None):
    data = {"type": "filament", "name": name}
    if inherits is not None:
        data["inherits"] = inherits
    if filament_id is not None:
        data["filament_id"] = filament_id
    if filament_vendor is not None:
        data["filament_vendor"] = (
            [filament_vendor] if isinstance(filament_vendor, str) else filament_vendor)
    if filament_type is not None:
        data["filament_type"] = (
            [filament_type] if isinstance(filament_type, str) else filament_type)
    data["instantiation"] = "true" if instantiation else "false"
    if compatible_printers is not None:
        data["compatible_printers"] = compatible_printers
    return data


class SyntheticTree:
    """A throwaway resources/profiles-shaped directory plus a snapshot path."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="filament_id_test_")
        self.profiles = os.path.join(self.dir, "profiles")
        os.makedirs(self.profiles)
        self.snapshot = os.path.join(self.dir, "filament_id_snapshot.json")

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def preset_path(self, vendor, name):
        return os.path.join(self.profiles, vendor, "filament", name + ".json")

    def add_vendor(self, vendor, presets):
        vendor_dir = os.path.join(self.profiles, vendor, "filament")
        os.makedirs(vendor_dir, exist_ok=True)
        index = {"name": vendor, "version": "01.00.00.00", "filament_list": []}
        for data in presets:
            fname = data["name"] + ".json"
            with open(os.path.join(vendor_dir, fname), "w", encoding="utf-8") as f:
                json.dump(data, f, indent=4, ensure_ascii=False)
            index["filament_list"].append(
                {"name": data["name"], "sub_path": f"filament/{fname}"})
        with open(os.path.join(self.profiles, vendor + ".json"), "w",
                  encoding="utf-8") as f:
            json.dump(index, f, indent=4, ensure_ascii=False)

    def add_to_index(self, vendor, name):
        idx_path = os.path.join(self.profiles, vendor + ".json")
        with open(idx_path, encoding="utf-8") as f:
            index = json.load(f)
        index["filament_list"].append(
            {"name": name, "sub_path": f"filament/{name}.json"})
        with open(idx_path, "w", encoding="utf-8") as f:
            json.dump(index, f, indent=4, ensure_ascii=False)

    def write_preset(self, vendor, data, register=True):
        path = self.preset_path(vendor, data["name"])
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=4, ensure_ascii=False)
        if register:
            self.add_to_index(vendor, data["name"])

    def remove_preset(self, vendor, name):
        os.remove(self.preset_path(vendor, name))
        idx_path = os.path.join(self.profiles, vendor + ".json")
        with open(idx_path, encoding="utf-8") as f:
            index = json.load(f)
        index["filament_list"] = [
            e for e in index["filament_list"] if e["name"] != name]
        with open(idx_path, "w", encoding="utf-8") as f:
            json.dump(index, f, indent=4, ensure_ascii=False)

    # -- pipeline wrappers ---------------------------------------------------

    def update_snapshot(self, allow_shared_catalog=False):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = afi.update_snapshot(self.profiles, self.snapshot,
                                     allow_shared_catalog=allow_shared_catalog)
        return rc, buf.getvalue()

    def check(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = afi.check_filament_ids(self.profiles, self.snapshot)
        return errors, buf.getvalue()

    def assign(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            changed, errors = afi.assign_missing_ids(self.profiles, self.snapshot)
        return changed, errors, buf.getvalue()

    def remint(self, vendors):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            changed, errors = afi.remint_vendors(vendors, self.profiles)
        return changed, errors, buf.getvalue()

    def drop_redundant(self, vendor):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            dropped, errors = afi.drop_redundant_ids(vendor, self.profiles)
        return dropped, errors, buf.getvalue()

def make_clean_tree():
    """Baseline tree: OFL base+generic, a vendor family, a clean tuned generic."""
    t = SyntheticTree()
    t.add_vendor(OFL, [
        preset("fdm_pla", filament_id="OGFL99", instantiation=False,
               filament_vendor="Generic", filament_type="PLA"),
        preset("Generic PLA @System", inherits="fdm_pla",
               compatible_printers=[]),
    ])
    t.add_vendor("VendorA", [
        preset("APLA @base", filament_id="AX01", instantiation=False,
               filament_vendor="AVendor", filament_type="PLA"),
        preset("APLA @P1", inherits="APLA @base",
               compatible_printers=["P1 0.4 nozzle"]),
        # Correctly tuned OFL generic: keeps the OFL base name, claims a printer.
        preset("Generic PLA @P1", inherits="Generic PLA @System",
               compatible_printers=["P1 0.4 nozzle"]),
    ])
    rc, _out = t.update_snapshot()
    assert rc == 0
    return t


class SyntheticTreeCase(unittest.TestCase):
    def setUp(self):
        self.t = make_clean_tree()
        self.addCleanup(self.t.cleanup)


# ---------------------------------------------------------------------------
# mint
# ---------------------------------------------------------------------------

class TestMint(unittest.TestCase):
    def test_namespace_literal(self):
        # Frozen: derived from the setting_id namespace; baked into the snapshot.
        self.assertEqual(afi.FILAMENT_ID_NAMESPACE,
                         uuid.UUID("c4d3ff49-4c32-5534-a3e3-00894157ab97"))

    def test_known_vector(self):
        # Hardcoded, independently computed vectors: freeze prefix, input string
        # layout ("filament_product/<vendor>/<type>/<family>") and base62 tail.
        self.assertEqual(afi.generate_filament_id("Polymaker", "PLA", "PolyLite PLA"),
                         "OF5CgdDq")
        self.assertEqual(afi.generate_filament_id("Polymaker", "PLA", "PolyLite PLA",
                                                  salt=1), "OFD9mV8H")
        self.assertEqual(afi.generate_filament_id("Generic", "PLA", "Generic PLA"),
                         "OFDSrzZ8")

    def test_determinism_and_format(self):
        for triple in [("Polymaker", "PLA", "PolyLite PLA"),
                       ("Creality", "PLA", "CR PLA"),
                       ("Generic", "ABS", "Generic ABS"),
                       ("拓竹", "PLA", "拓竹 PLA")]:  # unicode components
            a = afi.generate_filament_id(*triple)
            b = afi.generate_filament_id(*triple)
            self.assertEqual(a, b)
            self.assertRegex(a, r"^OF[0-9A-Za-z]{6}$")
            self.assertEqual(len(a), 8)

    def test_every_triple_component_changes_the_id(self):
        base = afi.generate_filament_id("Polymaker", "PLA", "PolyLite PLA")
        self.assertNotEqual(base, afi.generate_filament_id("Other", "PLA", "PolyLite PLA"))
        self.assertNotEqual(base, afi.generate_filament_id("Polymaker", "PETG", "PolyLite PLA"))
        self.assertNotEqual(base, afi.generate_filament_id("Polymaker", "PLA", "PolyLite PLA Pro"))

    def test_salt_changes_id(self):
        base = afi.generate_filament_id("Polymaker", "PLA", "PolyLite PLA")
        salted = afi.generate_filament_id("Polymaker", "PLA", "PolyLite PLA", salt=1)
        self.assertNotEqual(base, salted)
        self.assertRegex(salted, r"^OF[0-9A-Za-z]{6}$")

    def test_mint_salt_iteration(self):
        triple = ("Polymaker", "PLA", "PolyLite PLA")
        taken = {afi.generate_filament_id(*triple, salt=s) for s in range(2)}
        self.assertEqual(afi.mint_filament_id(*triple, taken),
                         afi.generate_filament_id(*triple, salt=2))
        self.assertEqual(afi.mint_filament_id(*triple, set()),
                         afi.generate_filament_id(*triple))


class TestBaseName(unittest.TestCase):
    def test_family_derivation(self):
        cases = [
            ("X @base", "X"),
            ("Afinia PLA@HS", "Afinia PLA"),
            ("PolyTerra PLA", "PolyTerra PLA"),
            ("HATCHBOX PLA @Qidi X-Plus 4 0.6 nozzle", "HATCHBOX PLA"),
            ("A @B @C", "A"),                      # first @ wins
            ("Filár PLA 拓竹 @0.4 nozzle", "Filár PLA 拓竹"),
        ]
        for name, family in cases:
            self.assertEqual(afi.base_name(name), family, msg=name)


# ---------------------------------------------------------------------------
# resolver (loader-faithful semantics)
# ---------------------------------------------------------------------------

class TestResolver(unittest.TestCase):
    @staticmethod
    def rec(name, filament_id=None, inherits=None):
        return {"name": name, "filament_id": filament_id, "inherits": inherits}

    def resolve(self, name, vendor_recs, ofl_recs, **kw):
        fmap = {r["name"]: r for r in vendor_recs}
        omap = {r["name"]: r for r in ofl_recs}
        return afi.resolve_filament_id(name, fmap, omap, **kw)

    def test_own_id(self):
        fid, src, entry = self.resolve("A", [self.rec("A", "ID1")], [])
        self.assertEqual((fid, src, entry), ("ID1", "own", None))

    def test_inherited_within_vendor(self):
        fid, src, entry = self.resolve(
            "A", [self.rec("A", inherits="B"), self.rec("B", inherits="C"),
                  self.rec("C", "ID3")], [])
        self.assertEqual((fid, src, entry), ("ID3", "inherited", None))

    def test_ofl_fallback(self):
        # Vendor preset inherits a name that only exists in the OFL map.
        fid, src, entry = self.resolve(
            "A", [self.rec("A", inherits="Generic PLA @System")],
            [self.rec("Generic PLA @System", inherits="fdm_pla"),
             self.rec("fdm_pla", "OGFL99")])
        self.assertEqual(fid, "OGFL99")
        self.assertEqual(entry, "Generic PLA @System")

    def test_ofl_stays_in_ofl(self):
        # Once a chain enters OFL it stays there: a vendor file sharing an
        # OFL-internal hop's name must not shadow it.
        fid, _src, entry = self.resolve(
            "A",
            [self.rec("A", inherits="ofl_entry"), self.rec("fdm_pla", "WRONG")],
            [self.rec("ofl_entry", inherits="fdm_pla"), self.rec("fdm_pla", "RIGHT")])
        self.assertEqual(fid, "RIGHT")
        self.assertEqual(entry, "ofl_entry")

    def test_dead_end_retries_parent_in_ofl(self):
        # The vendor chain dead-ends id-less on a parent that also exists in
        # OFL: the loader re-consults the OFL map for that direct parent.
        fid, _src, entry = self.resolve(
            "A", [self.rec("A", inherits="shared"), self.rec("shared")],
            [self.rec("shared", "OFLID1")])
        self.assertEqual(fid, "OFLID1")
        self.assertEqual(entry, "shared")

    def test_cycle(self):
        fid, src, _e = self.resolve(
            "A", [self.rec("A", inherits="B"), self.rec("B", inherits="A")], [])
        self.assertEqual((fid, src), (None, "cycle"))

    def test_dangling_parent(self):
        fid, src, _e = self.resolve("A", [self.rec("A", inherits="nope")], [])
        self.assertEqual((fid, src), (None, "dangling"))

    def test_missing_id(self):
        fid, src, _e = self.resolve("A", [self.rec("A")], [])
        self.assertEqual((fid, src), (None, "missing"))

    def test_skip_own_resolves_inherited(self):
        fid, _src, _e = self.resolve(
            "A", [self.rec("A", "OWN", inherits="B"), self.rec("B", "PARENT")], [],
            skip_own=True)
        self.assertEqual(fid, "PARENT")


class TestTripleResolution(unittest.TestCase):
    @staticmethod
    def rec(name, inherits=None, filament_vendor=None, filament_type=None):
        return {"name": name, "inherits": inherits,
                "filament_vendor": filament_vendor, "filament_type": filament_type}

    def field(self, name, vendor_recs, ofl_recs, field="filament_vendor"):
        fmap = {r["name"]: r for r in vendor_recs}
        omap = {r["name"]: r for r in ofl_recs}
        return afi.resolve_filament_field(name, field, fmap, omap)

    def test_own_value_first_element_of_list(self):
        got = self.field("A", [self.rec("A", filament_vendor=["Poly", "Ignored"])], [])
        self.assertEqual(got, "Poly")

    def test_plain_string_value_tolerated(self):
        got = self.field("A", [self.rec("A", filament_vendor="Poly")], [])
        self.assertEqual(got, "Poly")

    def test_inherited_within_vendor(self):
        got = self.field(
            "A", [self.rec("A", inherits="B"), self.rec("B", inherits="C"),
                  self.rec("C", filament_vendor=["Poly"])], [])
        self.assertEqual(got, "Poly")

    def test_empty_list_keeps_walking(self):
        got = self.field(
            "A", [self.rec("A", inherits="B", filament_vendor=[]),
                  self.rec("B", filament_vendor=["Poly"])], [])
        self.assertEqual(got, "Poly")

    def test_ofl_fallback(self):
        got = self.field(
            "A", [self.rec("A", inherits="Generic PLA @System")],
            [self.rec("Generic PLA @System", inherits="fdm_pla"),
             self.rec("fdm_pla", filament_vendor=["Generic"])])
        self.assertEqual(got, "Generic")

    def test_ofl_stays_in_ofl(self):
        got = self.field(
            "A",
            [self.rec("A", inherits="ofl_entry"),
             self.rec("fdm_pla", filament_vendor=["WRONG"])],
            [self.rec("ofl_entry", inherits="fdm_pla"),
             self.rec("fdm_pla", filament_vendor=["RIGHT"])])
        self.assertEqual(got, "RIGHT")

    def test_dead_end_retries_parent_in_ofl(self):
        got = self.field(
            "A", [self.rec("A", inherits="shared"), self.rec("shared")],
            [self.rec("shared", filament_vendor=["Poly"])])
        self.assertEqual(got, "Poly")

    def test_missing_is_empty(self):
        self.assertEqual(self.field("A", [self.rec("A")], []), "")
        self.assertEqual(self.field("A", [self.rec("A", inherits="nope")], []), "")

    def test_resolve_triple(self):
        recs = [self.rec("MyPLA @base", filament_vendor=["MyVendor"],
                         filament_type=["PLA"]),
                self.rec("MyPLA @P1", inherits="MyPLA @base")]
        fmap = {r["name"]: r for r in recs}
        self.assertEqual(afi.resolve_triple("MyPLA @P1", fmap, {}),
                         ("MyVendor", "PLA", "MyPLA"))


# ---------------------------------------------------------------------------
# reserved namespaces / islands
# ---------------------------------------------------------------------------

class TestReservedSpaces(unittest.TestCase):
    def test_owners(self):
        self.assertEqual(afi.reserved_space_owner("GFL99"), (True, "BBL"))
        # Qidi device protocol: reserved, but no vendor may declare it
        self.assertEqual(afi.reserved_space_owner("QD_X4_PLA"), (True, None))
        self.assertEqual(afi.reserved_space_owner("P1234abc"), (True, None))
        self.assertEqual(afi.reserved_space_owner("pAbCdEf1"), (True, None))  # case-insensitive
        self.assertEqual(afi.reserved_space_owner("null"), (True, None))
        self.assertEqual(afi.reserved_space_owner("OF5CgdDq"), (False, None))
        self.assertEqual(afi.reserved_space_owner("P1234abcd"), (False, None))  # 8 hex chars: not the user space

    def test_island_declarations(self):
        self.assertTrue(afi.is_island_declaration("BBL", "GFL99"))
        self.assertTrue(afi.is_island_declaration("BBL", "OF5CgdDq"))
        # Qidi presets mint like any other vendor's:
        self.assertFalse(afi.is_island_declaration("Qidi", "QD_X4_PLA"))
        self.assertFalse(afi.is_island_declaration("Qidi", "OF5CgdDq"))
        self.assertFalse(afi.is_island_declaration("VendorA", "GFL99"))


# ---------------------------------------------------------------------------
# checks on synthetic trees
# ---------------------------------------------------------------------------

class TestChecks(SyntheticTreeCase):
    def test_clean_tree_is_silent(self):
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)
        self.assertNotIn("[ERROR]", out)
        self.assertNotIn("[WARNING]", out)

    def test_check1_unknown_non_of_id(self):
        self.t.write_preset("VendorA", preset("BPLA @base", filament_id="BOGUS_9",
                                              instantiation=False,
                                              filament_vendor="BV", filament_type="PLA"))
        self.t.write_preset("VendorA", preset("BPLA @P1", inherits="BPLA @base",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("neither grandfathered in the snapshot", out)
        self.assertIn("BOGUS_9", out)

    def test_check2_new_claim_needs_snapshot_update(self):
        self.t.write_preset("VendorA", preset("ANEW @P2", inherits="APLA @base",
                                              compatible_printers=["P2"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn('claim "VendorA/ANEW" is not sanctioned', out)
        self.assertIn("--update-snapshot", out)

    def test_check2_vanished_claim_is_stability_error(self):
        self.t.remove_preset("VendorA", "APLA @P1")
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("stability", out)
        self.assertIn('"VendorA/APLA"', out)

    def test_check2_triple_change_needs_snapshot_update(self):
        self.t.write_preset("VendorA", preset("APLA @base", filament_id="AX01",
                                              instantiation=False,
                                              filament_vendor="AVendor",
                                              filament_type="PETG"),
                            register=False)
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn('triple "AVendor/PETG/APLA" is not sanctioned', out)
        self.assertIn('triple stability: snapshot triple "AVendor/PLA/APLA"', out)
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)

    def test_check3_of_id_must_match_triple_mint(self):
        self.t.write_preset("VendorA", preset("BNEW @base", filament_id="OFZZZZZZ",
                                              instantiation=False,
                                              filament_vendor="BV", filament_type="PLA"))
        self.t.write_preset("VendorA", preset("BNEW @P1", inherits="BNEW @base",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("does not match the mint of its triple", out)
        self.assertIn(afi.generate_filament_id("BV", "PLA", "BNEW"), out)

    def test_check3_salted_mint_is_accepted(self):
        salted = afi.generate_filament_id("BV", "PLA", "BNEW", salt=3)
        self.t.write_preset("VendorA", preset("BNEW @base", filament_id=salted,
                                              instantiation=False,
                                              filament_vendor="BV", filament_type="PLA"))
        self.t.write_preset("VendorA", preset("BNEW @P1", inherits="BNEW @base",
                                              compatible_printers=["P1"]))
        _errors, out = self.t.check()  # check 2 still wants a snapshot update
        self.assertNotIn("does not match the mint", out)

    def test_check3_grandfathered_id_triple_pair(self):
        # A non-conforming (id, triple) pair sanctioned by the snapshot passes.
        self.t.write_preset("VendorA", preset("CNEW @base", filament_id="OFZZZZZZ",
                                              instantiation=False,
                                              filament_vendor="CV", filament_type="PLA"))
        self.t.write_preset("VendorA", preset("CNEW @P1", inherits="CNEW @base",
                                              compatible_printers=["P1"]))
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)

    def test_check4_renamed_tuned_generic(self):
        self.t.write_preset("VendorA", preset("Tuned PLA @P1",
                                              inherits="Generic PLA @System",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("rename re-exposes the OFL preset", out)
        self.assertIn("Tuned PLA @P1", out)

    def test_check4_empty_compatible_printers(self):
        self.t.write_preset("VendorA", preset("Generic PLA @P2",
                                              inherits="Generic PLA @System",
                                              compatible_printers=[]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("cannot shadow the OFL preset", out)

    def test_check4_own_id_key_on_ofl_rider(self):
        # A vendor root that declares its own id while inheriting an OFL family
        # forks the family off the catalog id.
        self.t.write_preset("VendorA", preset("GPLA @base", filament_id="AX77",
                                              instantiation=False,
                                              inherits="Generic PLA @System"))
        self.t.write_preset("VendorA", preset("GPLA @P1", inherits="GPLA @base",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn('declares its own filament_id "AX77"', out)

    def test_check4_applies_to_non_generic_ofl_families(self):
        fid = afi.generate_filament_id("Polymaker", "PLA", "PolyLite PLA")
        self.t.write_preset(OFL, preset("poly_pla_base", filament_id=fid,
                                        instantiation=False,
                                        filament_vendor="Polymaker",
                                        filament_type="PLA"))
        self.t.write_preset(OFL, preset("PolyLite PLA @System",
                                        inherits="poly_pla_base",
                                        compatible_printers=[]))
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        self.t.write_preset("VendorA", preset("PolyX PLA @P1",
                                              inherits="PolyLite PLA @System",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("rename re-exposes the OFL preset", out)
        self.assertIn("PolyX PLA @P1", out)

    def test_check4_exception_is_grandfathered(self):
        self.t.write_preset("VendorA", preset("Tuned PLA @P1",
                                              inherits="Generic PLA @System",
                                              compatible_printers=["P1"]))
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)

    def test_check5_reserved_namespace_claims(self):
        for fid, marker in [("GFX99", "owned by BBL"),
                            ("QD_X_PLA", "composed by the device"),
                            ("P1a2b3c4", "user-custom"),
                            ("null", "user-custom")]:
            with self.subTest(fid=fid):
                name = f"R{fid} @base"
                self.t.write_preset("VendorA", preset(name, filament_id=fid,
                                                      instantiation=False,
                                                      filament_vendor="RV",
                                                      filament_type="PLA"))
                self.t.write_preset("VendorA", preset(f"R{fid} @P1", inherits=name,
                                                      compatible_printers=["P1"]))
                errors, out = self.t.check()
                self.assertGreater(errors, 0)
                self.assertIn("reserved id space", out)
                self.assertIn(marker, out)

    def test_check6a_instantiated_preset_with_own_key(self):
        self.t.write_preset("VendorA", preset("APLA @P1", filament_id="AX01",
                                              inherits="APLA @base",
                                              compatible_printers=["P1 0.4 nozzle"]),
                            register=False)
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("declares its own filament_id key", out)

    def test_check6b_declared_vs_inherited_drift(self):
        self.t.write_preset("VendorA", preset("APLA @P1", filament_id="AX02",
                                              inherits="APLA @base",
                                              compatible_printers=["P1 0.4 nozzle"]),
                            register=False)
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn('declares filament_id "AX02" but its inherits chain resolves "AX01"', out)

    def test_check6c_unresolvable_instantiated_filament(self):
        self.t.write_preset("VendorA", preset("DNEW @P1", compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("resolves no filament_id", out)
        self.assertIn("hard load error", out)

    def test_missing_snapshot_is_an_error(self):
        os.remove(self.t.snapshot)
        errors, out = self.t.check()
        self.assertEqual(errors, 1)
        self.assertIn("snapshot not found", out)


class TestCheck7(SyntheticTreeCase):
    def test_7a_empty_vendor_is_hard_error(self):
        fid = afi.generate_filament_id("", "PLA", "NVPLA")
        self.t.write_preset("VendorA", preset("NVPLA @base", filament_id=fid,
                                              instantiation=False,
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("NVPLA @P1", inherits="NVPLA @base",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("resolves empty filament_vendor", out)
        self.assertIn('filament_vendor "Generic"', out)
        # No grandfathering: sanctioning the tree does not silence check 8a.
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 1, out)
        self.assertIn("resolves empty filament_vendor", out)

    def test_7b_divergent_family_triples(self):
        id1 = afi.generate_filament_id("MV", "PLA", "MPLA")
        id2 = afi.generate_filament_id("MV", "PETG", "MPLA")
        self.t.write_preset("VendorA", preset("MPLA @base1", filament_id=id1,
                                              instantiation=False,
                                              filament_vendor="MV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("MPLA @base2", filament_id=id2,
                                              instantiation=False,
                                              filament_vendor="MV",
                                              filament_type="PETG"))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("divergent triples", out)
        self.assertIn("MV/PLA/MPLA", out)
        self.assertIn("MV/PETG/MPLA", out)
        # ... until --update-snapshot grandfathers the family in triple_exceptions.
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        self.assertIn("VendorA/MPLA", load_json_file(self.t.snapshot)["triple_exceptions"])
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)

    def test_7_cross_bundle_divergence_is_warning_only(self):
        fid = afi.generate_filament_id("BV", "PETG", "APLA")
        self.t.add_vendor("VendorB", [
            preset("APLA @base", filament_id=fid, instantiation=False,
                   filament_vendor="BV", filament_type="PETG"),
            preset("APLA @PB", inherits="APLA @base",
                   compatible_printers=["PB 0.4 nozzle"]),
        ])
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)
        self.assertIn("[WARNING]", out)
        self.assertIn("across bundles", out)
        self.assertIn('"APLA"', out)


# ---------------------------------------------------------------------------
# --update-snapshot
# ---------------------------------------------------------------------------

class TestUpdateSnapshot(SyntheticTreeCase):
    def test_idempotent_and_deterministic(self):
        with open(self.t.snapshot, "rb") as f:
            first = f.read()
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        self.assertIn("nothing changed", out)
        with open(self.t.snapshot, "rb") as f:
            self.assertEqual(f.read(), first)
        self.assertTrue(first.endswith(b"\n"))
        self.assertNotIn(b"\r", first)
        snap = json.loads(first.decode("utf-8"))
        self.assertEqual(list(snap["ids"]), sorted(snap["ids"]))
        self.assertEqual(snap["ids"]["AX01"], ["VendorA/APLA"])
        self.assertEqual(snap["ids"]["OGFL99"],
                         ["OrcaFilamentLibrary/Generic PLA", "VendorA/Generic PLA"])
        self.assertEqual(snap["triples"]["AX01"], [["AVendor", "PLA", "APLA"]])
        self.assertEqual(snap["triples"]["OGFL99"], [["Generic", "PLA", "fdm_pla"]])
        self.assertEqual(snap["triple_exceptions"], [])

    def test_refuses_new_reserved_namespace_claims(self):
        self.t.write_preset("VendorA", preset("CNEW @base", filament_id="GFX99",
                                              instantiation=False,
                                              filament_vendor="CV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("CNEW @P1", inherits="CNEW @base",
                                              compatible_printers=["P1"]))
        with open(self.t.snapshot, "rb") as f:
            before = f.read()
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 1)
        self.assertIn("refusing to sanction", out)
        with open(self.t.snapshot, "rb") as f:
            self.assertEqual(f.read(), before)  # nothing written on refusal
        rc, _out = self.t.update_snapshot(allow_shared_catalog=True)
        self.assertEqual(rc, 0)
        snap = load_json_file(self.t.snapshot)
        self.assertEqual(snap["ids"]["GFX99"], ["VendorA/CNEW"])


# ---------------------------------------------------------------------------
# default run: mint + insert
# ---------------------------------------------------------------------------

class TestAssign(SyntheticTreeCase):
    def test_noop_on_fully_idded_tree(self):
        changed, errors, out = self.t.assign()
        self.assertEqual((changed, errors), (0, 0))
        self.assertIn("nothing to do (0 files changed)", out)

    def test_mints_into_family_root_and_rootless_member(self):
        self.t.write_preset("VendorA", preset("FNEW @base", instantiation=False,
                                              filament_vendor="FV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("FNEW @P1", inherits="FNEW @base",
                                              compatible_printers=["P1"]))
        self.t.write_preset("VendorA", preset("FNEW @P2", inherits="FNEW @base",
                                              compatible_printers=["P2"]))
        self.t.write_preset("VendorA", preset("GNEW @P1", compatible_printers=["P1"],
                                              filament_vendor="GV",
                                              filament_type="PETG"))
        changed, errors, _out = self.t.assign()
        self.assertEqual(errors, 0)
        self.assertEqual(changed, 2)  # one root + one root-less member
        root = load_json_file(self.t.preset_path("VendorA", "FNEW @base"))
        self.assertEqual(root["filament_id"],
                         afi.generate_filament_id("FV", "PLA", "FNEW"))
        member = load_json_file(self.t.preset_path("VendorA", "GNEW @P1"))
        self.assertEqual(member["filament_id"],
                         afi.generate_filament_id("GV", "PETG", "GNEW"))
        # variants themselves never get the key
        child = load_json_file(self.t.preset_path("VendorA", "FNEW @P1"))
        self.assertNotIn("filament_id", child)
        # idempotent: second run is a no-op
        changed, errors, out = self.t.assign()
        self.assertEqual((changed, errors), (0, 0))
        self.assertIn("nothing to do", out)

    def test_refuses_family_with_incomplete_triple(self):
        self.t.write_preset("VendorA", preset("ENEW @base", instantiation=False))
        self.t.write_preset("VendorA", preset("ENEW @P1", inherits="ENEW @base",
                                              compatible_printers=["P1"]))
        changed, errors, out = self.t.assign()
        self.assertEqual(changed, 0)
        self.assertGreater(errors, 0)
        self.assertIn("resolves empty filament_vendor and filament_type", out)
        self.assertIn("mint key needs both", out)

    def test_refuses_family_with_divergent_root_fields(self):
        self.t.write_preset("VendorA", preset("HNEW @base1", instantiation=False,
                                              filament_vendor="HV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("HNEW @base2", instantiation=False,
                                              filament_vendor="HV",
                                              filament_type="PETG"))
        self.t.write_preset("VendorA", preset("HNEW @P1", inherits="HNEW @base1",
                                              compatible_printers=["P1"]))
        self.t.write_preset("VendorA", preset("HNEW @P2", inherits="HNEW @base2",
                                              compatible_printers=["P2"]))
        changed, errors, out = self.t.assign()
        self.assertEqual(changed, 0)
        self.assertGreater(errors, 0)
        self.assertIn("divergent (filament_vendor, filament_type)", out)

    def test_shared_root_between_families_is_refused(self):
        self.t.write_preset("VendorA", preset("shared_base", instantiation=False,
                                              filament_vendor="SV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("HNEW @P1", inherits="shared_base",
                                              compatible_printers=["P1"]))
        self.t.write_preset("VendorA", preset("INEW @P1", inherits="shared_base",
                                              compatible_printers=["P1"]))
        changed, errors, out = self.t.assign()
        self.assertEqual(changed, 0)
        self.assertGreater(errors, 0)
        self.assertIn("shared with famil", out)


# ---------------------------------------------------------------------------
# byte-preserving profile edits
# ---------------------------------------------------------------------------

class TestInsertEditing(unittest.TestCase):
    CRLF_TEXT = (
        '{\r\n'
        '\t"type": "filament",\r\n'
        '\t"name": "JNEW @base",\r\n'
        '\t"inherits": "fdm_pla",\r\n'
        '\t"from": "system",\r\n'
        '\t"instantiation": "false",\r\n'
        '\t"filament_type": [\r\n'
        '\t\t"PLA"\r\n'
        '\t]\r\n'
        '}\r\n'
    )
    CRLF_WITH_ID = (
        '{\r\n'
        '\t"type": "filament",\r\n'
        '\t"name": "JNEW @base",\r\n'
        '\t"filament_id": "OFold123",\r\n'
        '\t"instantiation": "false",\r\n'
        '\t"filament_type": [\r\n'
        '\t\t"PLA"\r\n'
        '\t]\r\n'
        '}\r\n'
    )

    def test_insert_before_instantiation_preserves_bytes(self):
        text, n = afi.insert_filament_id(self.CRLF_TEXT, "OFabc123")
        self.assertEqual(n, 1)
        json.loads(text)
        inserted = '\t"filament_id": "OFabc123",\r\n'
        self.assertIn(inserted + '\t"instantiation"', text)
        # every original byte is preserved: removing the inserted line restores
        # the input exactly (CRLF stays CRLF, tabs stay tabs)
        self.assertEqual(text.replace(inserted, "", 1), self.CRLF_TEXT)

    def test_insert_after_name_when_no_instantiation_line(self):
        lf_text = '{\n    "type": "filament",\n    "name": "K",\n    "from": "system"\n}\n'
        text, n = afi.insert_filament_id(lf_text, "OFabc123")
        self.assertEqual(n, 1)
        json.loads(text)
        self.assertIn('"name": "K",\n    "filament_id": "OFabc123",\n', text)
        self.assertNotIn("\r", text)

    def test_insert_no_anchor_fails(self):
        _text, n = afi.insert_filament_id('{"type": "filament"}', "OFabc123")
        self.assertEqual(n, 0)

    def test_write_filament_id_keeps_crlf_on_disk(self):
        t = SyntheticTree()
        self.addCleanup(t.cleanup)
        t.add_vendor("VendorA", [])
        path = t.preset_path("VendorA", "JNEW @base")
        with open(path, "wb") as f:
            f.write(self.CRLF_TEXT.encode("utf-8"))
        t.add_to_index("VendorA", "JNEW @base")
        afi.write_filament_id(path, "OFabc123")
        with open(path, "rb") as f:
            raw = f.read()
        self.assertEqual(raw.count(b"\n"), raw.count(b"\r\n"))  # still CRLF-only
        self.assertEqual(
            raw.replace(b'\t"filament_id": "OFabc123",\r\n', b"", 1),
            self.CRLF_TEXT.encode("utf-8"))

    def test_replace_value_preserves_every_other_byte(self):
        text, n = afi.replace_filament_id_value(self.CRLF_WITH_ID,
                                                "OFold123", "OFnew456")
        self.assertEqual(n, 1)
        self.assertEqual(json.loads(text)["filament_id"], "OFnew456")
        self.assertEqual(text, self.CRLF_WITH_ID.replace('"OFold123"', '"OFnew456"'))

    def test_replace_value_requires_exact_old_value(self):
        _text, n = afi.replace_filament_id_value(self.CRLF_WITH_ID,
                                                 "OFwrong1", "OFnew456")
        self.assertEqual(n, 0)

    def test_rewrite_filament_id_on_disk(self):
        t = SyntheticTree()
        self.addCleanup(t.cleanup)
        t.add_vendor("VendorA", [])
        path = t.preset_path("VendorA", "JNEW @base")
        with open(path, "wb") as f:
            f.write(self.CRLF_WITH_ID.encode("utf-8"))
        afi.rewrite_filament_id(path, "OFold123", "OFnew456")
        with open(path, "rb") as f:
            raw = f.read()
        self.assertEqual(raw,
                         self.CRLF_WITH_ID.replace('"OFold123"', '"OFnew456"')
                         .encode("utf-8"))
        with self.assertRaises(RuntimeError):
            afi.rewrite_filament_id(path, "OFold123", "OFxxx999")  # stale old id

    def test_delete_line_with_trailing_comma(self):
        text, n = afi.delete_filament_id_line(self.CRLF_WITH_ID, "OFold123")
        self.assertEqual(n, 1)
        self.assertNotIn("filament_id", json.loads(text))
        self.assertEqual(text, self.CRLF_WITH_ID.replace(
            '\t"filament_id": "OFold123",\r\n', "", 1))

    def test_delete_last_property_line(self):
        lf_text = ('{\n    "name": "K @base",\n    "instantiation": "false",\n'
                   '    "filament_id": "OFold123"\n}\n')
        text, n = afi.delete_filament_id_line(lf_text, "OFold123")
        self.assertEqual(n, 1)
        data = json.loads(text)
        self.assertNotIn("filament_id", data)
        self.assertEqual(data["instantiation"], "false")


# ---------------------------------------------------------------------------
# --remint
# ---------------------------------------------------------------------------

class TestRemint(SyntheticTreeCase):
    TRIPLE = ("AVendor", "PLA", "APLA")

    def test_rewrites_mismatching_declaration(self):
        want = afi.generate_filament_id(*self.TRIPLE)
        changed, errors, out = self.t.remint(["VendorA"])
        self.assertEqual(errors, 0, out)
        self.assertEqual(changed, 1)
        self.assertIn('"AX01" -> "%s"' % want, out)
        root = load_json_file(self.t.preset_path("VendorA", "APLA @base"))
        self.assertEqual(root["filament_id"], want)
        # OFL was not part of the run
        ofl_root = load_json_file(self.t.preset_path(OFL, "fdm_pla"))
        self.assertEqual(ofl_root["filament_id"], "OGFL99")
        # idempotent: a second run over the same vendor rewrites nothing
        changed, errors, _out = self.t.remint(["VendorA"])
        self.assertEqual((changed, errors), (0, 0))

    def test_salted_declaration_survives_remint(self):
        # A deliberate salt split (a second preset of one product kept on its
        # own id for per-printer AMS disambiguation) is mint-conformant and
        # must not be converged back onto salt 0.
        salt1 = afi.generate_filament_id(*self.TRIPLE, salt=1)
        self.t.write_preset("VendorA", preset("APLA @legacy",
                                              filament_id=salt1,
                                              inherits="APLA @base",
                                              compatible_printers=["P2 0.4 nozzle"]))
        changed, errors, out = self.t.remint(["VendorA"])
        self.assertEqual(errors, 0, out)
        self.assertEqual(changed, 1)  # only the non-conformant AX01 root
        legacy = load_json_file(self.t.preset_path("VendorA", "APLA @legacy"))
        self.assertEqual(legacy["filament_id"], salt1)

    def test_same_triple_converges_within_run(self):
        self.t.add_vendor("VendorB", [
            preset("APLA @Bbase", filament_id="BX01", instantiation=False,
                   filament_vendor="AVendor", filament_type="PLA"),
            preset("APLA @PB", inherits="APLA @Bbase",
                   compatible_printers=["PB 0.4 nozzle"]),
        ])
        want = afi.generate_filament_id(*self.TRIPLE)
        changed, errors, _out = self.t.remint(["VendorA", "VendorB"])
        self.assertEqual(errors, 0)
        self.assertEqual(changed, 2)
        a = load_json_file(self.t.preset_path("VendorA", "APLA @base"))
        b = load_json_file(self.t.preset_path("VendorB", "APLA @Bbase"))
        self.assertEqual(a["filament_id"], want)
        self.assertEqual(b["filament_id"], want)

    def test_same_triple_converges_with_existing_tree_id(self):
        # Another bundle already carries the conforming id for the same triple:
        # reuse is required, not blocked.
        want = afi.generate_filament_id(*self.TRIPLE)
        self.t.add_vendor("VendorB", [
            preset("APLA @Bbase", filament_id=want, instantiation=False,
                   filament_vendor="AVendor", filament_type="PLA"),
        ])
        changed, errors, _out = self.t.remint(["VendorA"])
        self.assertEqual((changed, errors), (1, 0))
        a = load_json_file(self.t.preset_path("VendorA", "APLA @base"))
        self.assertEqual(a["filament_id"], want)

    def test_blocked_by_other_triple_occurrence(self):
        want0 = afi.generate_filament_id(*self.TRIPLE)
        self.t.add_vendor("VendorB", [
            preset("BPLA @base", filament_id=want0, instantiation=False,
                   filament_vendor="BV", filament_type="PETG"),
        ])
        changed, errors, _out = self.t.remint(["VendorA"])
        self.assertEqual((changed, errors), (1, 0))
        root = load_json_file(self.t.preset_path("VendorA", "APLA @base"))
        self.assertEqual(root["filament_id"],
                         afi.generate_filament_id(*self.TRIPLE, salt=1))

    def test_bbl_is_forbidden(self):
        changed, errors, out = self.t.remint(["BBL"])
        self.assertEqual(changed, 0)
        self.assertGreater(errors, 0)
        self.assertIn("forbidden", out)


# ---------------------------------------------------------------------------
# --drop-redundant-ids
# ---------------------------------------------------------------------------

class TestDropRedundantIds(SyntheticTreeCase):
    def test_drops_only_ofl_riding_same_base_name_declarations(self):
        # Redundant: same base name, rides the OFL generic, re-declares its id.
        self.t.write_preset("VendorA", preset("Generic PLA @P2",
                                              inherits="Generic PLA @System",
                                              compatible_printers=["P2"],
                                              filament_id="OGFL99"))
        # Renamed rider: not dropped (a repoint worksheet decision, not a drop).
        self.t.write_preset("VendorA", preset("Tuned PLA @P3",
                                              inherits="Generic PLA @System",
                                              compatible_printers=["P3"],
                                              filament_id="OGFL99"))
        dropped, errors, out = self.t.drop_redundant("VendorA")
        self.assertEqual(errors, 0, out)
        self.assertEqual(dropped, 1)
        self.assertIn('dropped filament_id "OGFL99"', out)
        self.assertIn('rides OFL "Generic PLA @System"', out)
        gone = load_json_file(self.t.preset_path("VendorA", "Generic PLA @P2"))
        self.assertNotIn("filament_id", gone)
        kept = load_json_file(self.t.preset_path("VendorA", "Tuned PLA @P3"))
        self.assertEqual(kept["filament_id"], "OGFL99")
        # vendor-rooted families are untouched
        root = load_json_file(self.t.preset_path("VendorA", "APLA @base"))
        self.assertEqual(root["filament_id"], "AX01")

    def test_bbl_is_forbidden(self):
        dropped, errors, out = self.t.drop_redundant("BBL")
        self.assertEqual(dropped, 0)
        self.assertGreater(errors, 0)
        self.assertIn("forbidden", out)


# ---------------------------------------------------------------------------
# --mint CLI
# ---------------------------------------------------------------------------

class TestMintCli(unittest.TestCase):
    def test_prints_an_of_id(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = afi.main(["--mint", "Polymaker/PLA/PolyLite PLA"])
        self.assertEqual(rc, 0)
        self.assertRegex(buf.getvalue().strip(), r"^OF[0-9A-Za-z]{6}$")

    def test_requires_exactly_three_parts(self):
        with self.assertRaises(SystemExit), \
                contextlib.redirect_stderr(io.StringIO()):
            afi.main(["--mint", "Vendor/Family"])


# ---------------------------------------------------------------------------
# the real tree
# ---------------------------------------------------------------------------

@unittest.skipUnless(os.path.isdir(REAL_PROFILES), "resources/profiles not present")
class TestRealTree(unittest.TestCase):
    def test_shipped_snapshot_matches_tree(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = afi.check_filament_ids(REAL_PROFILES)
        self.assertEqual(errors, 0, buf.getvalue())

    def test_every_instantiated_filament_resolves_an_id(self):
        analysis = afi.analyze_tree(REAL_PROFILES)
        self.assertEqual(analysis["missing_effective"], [])
        self.assertEqual(analysis["read_errors"], [])

# ---------------------------------------------------------------------------
# review-fix regressions
# ---------------------------------------------------------------------------

class TestReviewFixes(SyntheticTreeCase):
    def test_check3_skips_of_id_inherited_from_other_vendor(self):
        # An OFL family carries its own minted OF id and a vendor tunes it
        # correctly (same base name, non-empty printers). The new claim must
        # trip only the snapshot gate, never mint conformance.
        fid = afi.generate_filament_id("Generic", "PLA", "Generic PLA Matte")
        self.t.write_preset(OFL, preset("Generic PLA Matte @base", filament_id=fid,
                                        instantiation=False,
                                        filament_vendor="Generic",
                                        filament_type="PLA"))
        self.t.write_preset(OFL, preset("Generic PLA Matte @System",
                                        inherits="Generic PLA Matte @base",
                                        compatible_printers=[]))
        rc, _ = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        self.t.write_preset("VendorA", preset("Generic PLA Matte @P1",
                                              inherits="Generic PLA Matte @System",
                                              compatible_printers=["P1 0.4 nozzle"]))
        errors, out = self.t.check()
        self.assertNotIn("does not match the mint", out)
        self.assertIn("not sanctioned", out)
        self.assertEqual(errors, 1, out)
        # After sanctioning the claim the tree is fully green again.
        rc, _ = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)

    def test_check6c_prints_expected_mint(self):
        self.t.write_preset("VendorA", preset("Orphan PLA @P1",
                                              compatible_printers=["P1 0.4 nozzle"],
                                              filament_vendor="OV",
                                              filament_type="PLA"))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn(afi.generate_filament_id("OV", "PLA", "Orphan PLA"), out)


if __name__ == "__main__":
    unittest.main()
