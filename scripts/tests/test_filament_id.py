#!/usr/bin/env python3
"""Tests for scripts/orca_id_tool.py (stdlib unittest, no external deps).

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

import orca_id_tool as afi  # noqa: E402
import update_bambu_filament_ids as ubfi  # noqa: E402

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

    def set_sub_path(self, vendor, name, sub_path):
        """Rewrite one index entry's sub_path (the file itself does not move)."""
        idx_path = os.path.join(self.profiles, vendor + ".json")
        with open(idx_path, encoding="utf-8") as f:
            index = json.load(f)
        for entry in index["filament_list"]:
            if entry["name"] == name:
                entry["sub_path"] = sub_path
        with open(idx_path, "w", encoding="utf-8") as f:
            json.dump(index, f, indent=4, ensure_ascii=False)

    def remove_preset(self, vendor, name):
        os.remove(self.preset_path(vendor, name))
        idx_path = os.path.join(self.profiles, vendor + ".json")
        with open(idx_path, encoding="utf-8") as f:
            index = json.load(f)
        index["filament_list"] = [
            e for e in index["filament_list"] if e["name"] != name]
        with open(idx_path, "w", encoding="utf-8") as f:
            json.dump(index, f, indent=4, ensure_ascii=False)

    def bytes_map(self):
        """{relative path -> file bytes} over every .json in the tree."""
        raw = {}
        for root, dirs, files in os.walk(self.profiles):
            dirs.sort()
            for name in sorted(files):
                if not name.endswith(".json"):
                    continue
                path = os.path.join(root, name)
                with open(path, "rb") as f:
                    raw[os.path.relpath(path, self.profiles)] = f.read()
        return raw

    # -- pipeline wrappers ---------------------------------------------------

    def update_snapshot(self, dry_run=False):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = afi.update_snapshot(self.profiles, self.snapshot, dry_run)
        return rc, buf.getvalue()

    def check(self, map_path=None):
        buf = io.StringIO()
        kwargs = {} if map_path is None else {"map_path": map_path}
        with contextlib.redirect_stdout(buf):
            errors = afi.check_filament_ids(self.profiles, self.snapshot, **kwargs)
        return errors, buf.getvalue()

    # assign() and remint() are the same one pass over the tree — every filament
    # ends up with the id its own triple mints, whether that means inserting a
    # missing key or rewriting a non-conformant one. Both names are kept because
    # they read differently at the call sites.
    def assign(self, vendors=None, dry_run=False):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            changed, errors = afi.generate_filament_ids(self.profiles, vendors, dry_run)
        return changed, errors, buf.getvalue()

    def remint(self, vendors, dry_run=False):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            changed, errors = afi.generate_filament_ids(self.profiles, vendors, dry_run)
        return changed, errors, buf.getvalue()

    def cli(self, *flags):
        """Run main() against this tree, capturing stdout."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = afi.main([*flags, "--profiles", self.profiles,
                           "--snapshot", self.snapshot])
        return rc, buf.getvalue()


def make_clean_tree(apla_id="AX01", generic_id="OGFL99"):
    """Baseline tree: OFL base+generic, a vendor filament, a clean tuned generic.

    apla_id/generic_id default to arbitrary non-OF placeholders (sanctioned by
    the snapshot below) since most tests only need "already assigned, don't
    touch" and never run the checks. TestAssign and the check tests pass real
    OF-format ids instead (OfCleanTreeCase).
    """
    t = SyntheticTree()
    t.add_vendor(OFL, [
        preset("Generic PLA @base", filament_id=generic_id, instantiation=False,
               filament_vendor="Generic", filament_type="PLA"),
        preset("Generic PLA @System", inherits="Generic PLA @base",
               compatible_printers=[]),
    ])
    t.add_vendor("VendorA", [
        preset("APLA @base", filament_id=apla_id, instantiation=False,
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


class OfCleanTreeCase(unittest.TestCase):
    """Like SyntheticTreeCase, but the baseline filament/generic already carry
    real OF-format ids (check 1 now rejects "AX01"/"OGFL99" unconditionally,
    with no snapshot exemption), so an otherwise-untouched tree still passes
    check_filament_ids. Tests that specifically need a non-OF baseline to
    remint (TestRemint, TestUpdateSnapshot) keep using SyntheticTreeCase
    instead.
    """
    def setUp(self):
        self.t = make_clean_tree(
            apla_id=afi.generate_filament_id("AVendor", "PLA", "APLA"),
            generic_id=afi.generate_filament_id("Generic", "PLA", "Generic PLA"))
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
        # layout ("filament_product/<vendor>/<type>/<name>") and base62 tail.
        self.assertEqual(afi.generate_filament_id("Polymaker", "PLA", "PolyLite PLA"),
                         "OF5CgdDq")
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


class TestBaseName(unittest.TestCase):
    def test_filament_name_derivation(self):
        cases = [
            ("X @base", "X"),
            ("Afinia PLA@HS", "Afinia PLA"),
            ("PolyTerra PLA", "PolyTerra PLA"),
            ("HATCHBOX PLA @Qidi X-Plus 4 0.6 nozzle", "HATCHBOX PLA"),
            ("A @B @C", "A"),                      # first @ wins
            ("Filár PLA 拓竹 @0.4 nozzle", "Filár PLA 拓竹"),
        ]
        for name, filament_name in cases:
            self.assertEqual(afi.base_name(name), filament_name, msg=name)


# ---------------------------------------------------------------------------
# vendor discovery
# ---------------------------------------------------------------------------

class TestVendorDiscovery(unittest.TestCase):
    def test_a_bundle_is_a_subdir_with_a_matching_index(self):
        # Neither half alone makes a bundle: resources/profiles tracks a "user"
        # directory with no user.json, and an index without its directory is a
        # leftover. Both are skipped, tree-wide.
        t = SyntheticTree()
        self.addCleanup(t.cleanup)
        t.add_vendor("VendorA", [])
        os.makedirs(os.path.join(t.profiles, "user", "filament"))
        with open(os.path.join(t.profiles, "Orphan.json"), "w",
                  encoding="utf-8") as f:
            json.dump({"name": "Orphan", "filament_list": []}, f)
        self.assertEqual(afi.list_vendor_names(t.profiles), ["VendorA"])
        self.assertEqual(sorted(afi.analyze_tree(t.profiles)["vendors"]),
                         ["VendorA"])


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
# reserved namespaces
# ---------------------------------------------------------------------------

class TestReservedSpaces(unittest.TestCase):
    def test_owners(self):
        # Bambu AMS/RFID catalog: reserved, but no vendor (not even BBL) may declare it
        self.assertEqual(afi.reserved_space_owner("GFL99"), (True, None))
        # Qidi device protocol: reserved, but no vendor may declare it
        self.assertEqual(afi.reserved_space_owner("QD_X4_PLA"), (True, None))
        self.assertEqual(afi.reserved_space_owner("P1234abc"), (True, None))
        self.assertEqual(afi.reserved_space_owner("pAbCdEf1"), (True, None))  # case-insensitive
        self.assertEqual(afi.reserved_space_owner("null"), (True, None))
        self.assertEqual(afi.reserved_space_owner("OF5CgdDq"), (False, None))
        self.assertEqual(afi.reserved_space_owner("P1234abcd"), (False, None))  # 8 hex chars: not the user space

    def test_gf_is_reserved_and_ownerless(self):
        self.assertEqual(afi.reserved_space_owner("GFA00"), (True, None))


# ---------------------------------------------------------------------------
# checks on synthetic trees
# ---------------------------------------------------------------------------

class TestChecks(OfCleanTreeCase):
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
        self.assertIn('is not a minted "OF" id', out)
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
        apla_id = afi.generate_filament_id("AVendor", "PLA", "APLA")
        self.t.write_preset("VendorA", preset("APLA @base", filament_id=apla_id,
                                              instantiation=False,
                                              filament_vendor="AVendor",
                                              filament_type="PETG"),
                            register=False)
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn('triple "AVendor/PETG/APLA" is not sanctioned', out)
        self.assertIn('which records "AVendor/PLA/APLA"', out)
        # Sanctioning the new triple is not enough: the old id is no longer its
        # mint (check 3, nothing grandfathered) — the identity fix is a re-mint,
        # reported on the root and again under the variant inheriting it.
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 2, out)
        self.assertIn("does not match the mint of its triple", out)
        self.assertIn('"APLA @P1" (VendorA/filament/APLA @P1.json) inherits filament_id', out)
        _changed, errors, out = self.t.remint(["VendorA"])
        self.assertEqual(errors, 0, out)
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

    def test_check3_no_grandfathering_of_a_wrong_declaration(self):
        # Sanctioning the tree does not excuse a declaration from its mint.
        self.t.write_preset("VendorA", preset("CNEW @base", filament_id="OFZZZZZZ",
                                              instantiation=False,
                                              filament_vendor="CV", filament_type="PLA"))
        self.t.write_preset("VendorA", preset("CNEW @P1", inherits="CNEW @base",
                                              compatible_printers=["P1"]))
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 2, out)  # the declaration, and the variant inheriting it
        self.assertIn("does not match the mint of its triple", out)
        self.assertIn('"CNEW @P1" (VendorA/filament/CNEW @P1.json) inherits filament_id', out)

    def test_check3_inherited_id_must_be_the_mint_of_own_triple(self):
        # A preset of another filament inheriting APLA's root takes APLA's id,
        # which is not the mint of ITS triple (AVendor/PLA/Tuned PLA).
        self.t.write_preset("VendorA", preset("Tuned PLA @P1", inherits="APLA @base",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn('"Tuned PLA @P1" (VendorA/filament/Tuned PLA @P1.json) inherits '
                      'filament_id "%s"' % afi.generate_filament_id("AVendor", "PLA", "APLA"),
                      out)
        self.assertIn('mints "%s"' % afi.generate_filament_id("AVendor", "PLA", "Tuned PLA"),
                      out)
        # ... and sanctioning the tree does not excuse it either.
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 1, out)

    def test_check3_lists_every_preset_inheriting_a_wrong_id(self):
        # A wrong declaration is reported under every preset inheriting it, its
        # own product's variant and another product alike: each one's effective
        # id is not the mint of its own triple, and each is listed. Nothing is
        # folded into the declarer's error.
        self.t.write_preset("VendorA", preset("DNEW @base", filament_id="OFZZZZZZ",
                                              instantiation=False,
                                              filament_vendor="DV", filament_type="PLA"))
        self.t.write_preset("VendorA", preset("DNEW @P1", inherits="DNEW @base",
                                              compatible_printers=["P1"]))
        self.t.write_preset("VendorA", preset("Other DNEW @P1", inherits="DNEW @base",
                                              compatible_printers=["P1 0.4 nozzle"]))
        errors, out = self.t.check()
        self.assertIn("does not match the mint of its triple", out)
        self.assertIn('"DNEW @P1" (VendorA/filament/DNEW @P1.json) inherits filament_id', out)
        self.assertIn('"Other DNEW @P1" (VendorA/filament/Other DNEW @P1.json) inherits '
                      'filament_id', out)
        # The unsanctioned id (check 2), the declaration (3a), and both presets
        # inheriting it (3b).
        self.assertEqual(errors, 4, out)

    def test_check3_reports_an_inherited_mismatch_even_when_its_own_product_misdeclares_the_id(self):
        # "Tuned PLA @P1" inherits APLA's root, so it carries APLA's id: wrong
        # for its own product however the declarations around it are fixed.
        # That "Tuned PLA @base" — its own product — misdeclares that same id
        # is a second error, not a reason to leave the first unreported.
        apla_id = afi.generate_filament_id("AVendor", "PLA", "APLA")
        self.t.write_preset("VendorA", preset("Tuned PLA @base", filament_id=apla_id,
                                              instantiation=False,
                                              filament_vendor="AVendor",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("Tuned PLA @P1", inherits="APLA @base",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertIn('"Tuned PLA @base" (VendorA/filament/Tuned PLA @base.json) does '
                      'not match the mint of its triple', out)
        self.assertIn('"Tuned PLA @P1" (VendorA/filament/Tuned PLA @P1.json) inherits '
                      'filament_id', out)
        # The unsanctioned claim and triple (check 2), the declaration (3a) and
        # the inherited id (3b): four distinct errors, nothing folded away.
        self.assertEqual(errors, 4, out)

    def test_check3_reports_a_collision_between_two_products(self):
        # Two products whose triples mint one id is a base62 collision. There
        # is no salted or hand-picked second id to fall back on: the check
        # names both products, and the remedy is a rename so the triples differ.
        collide = {("V", "PLA", "X"), ("W", "ABS", "Y")}
        real = afi.generate_filament_id

        def colliding(vendor, ftype, name):
            return "OFcolid0" if (vendor, ftype, name) in collide else real(vendor, ftype, name)

        afi.generate_filament_id = colliding
        self.addCleanup(setattr, afi, "generate_filament_id", real)
        for vendor, ftype, name in sorted(collide):
            self.t.write_preset("VendorA", preset(f"{name} @base", filament_id="OFcolid0",
                                                  instantiation=False,
                                                  filament_vendor=vendor,
                                                  filament_type=ftype))
            self.t.write_preset("VendorA", preset(f"{name} @P1", inherits=f"{name} @base",
                                                  compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertIn("collision", out)
        self.assertIn("V/PLA/X", out)
        self.assertIn("W/ABS/Y", out)
        # Each declaration is the mint of its own triple, so the collision is
        # the only identity error — no product is pushed off its id — and the
        # unsanctioned id (check 2) is the only other one.
        self.assertNotIn("does not match the mint", out)
        self.assertNotIn("inherits filament_id", out)
        self.assertEqual(errors, 2, out)

    def test_check3_renamed_tuned_generic_is_an_identity_error(self):
        # Riding the OFL generic under another base name: same rule, same error.
        self.t.write_preset("VendorA", preset("Tuned PLA @P1",
                                              inherits="Generic PLA @System",
                                              compatible_printers=["P1"]))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("Tuned PLA @P1", out)
        self.assertIn("inherits filament_id", out)

    def test_check3_own_key_on_an_instantiated_preset_is_fine(self):
        # Where the id comes from is irrelevant: a variant may carry the key.
        apla_id = afi.generate_filament_id("AVendor", "PLA", "APLA")
        self.t.write_preset("VendorA", preset("APLA @P1", filament_id=apla_id,
                                              inherits="APLA @base",
                                              compatible_printers=["P1 0.4 nozzle"]),
                            register=False)
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)

    def test_check3_inheriting_a_real_filament_of_another_product_is_fine(self):
        # A branded product may inherit the OFL generic (an instantiated
        # preset) for its settings; it declares its own triple's id.
        fid = afi.generate_filament_id("BV", "PLA", "Branded PLA")
        self.t.write_preset("VendorA", preset("Branded PLA @P1", filament_id=fid,
                                              inherits="Generic PLA @System",
                                              filament_vendor="BV",
                                              compatible_printers=["P1"]))
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)
        # With a wrong key it is a plain mint mismatch: the parent plays no
        # part in the verdict.
        drifted = afi.generate_filament_id("AVendor", "PLA", "APLA")
        self.t.write_preset("VendorA", preset("Branded PLA @P1", filament_id=drifted,
                                              inherits="Generic PLA @System",
                                              filament_vendor="BV",
                                              compatible_printers=["P1"]),
                            register=False)
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn("does not match the mint of its triple", out)

    def test_check4_reserved_namespace_claims(self):
        for fid, marker in [("GFX99", "Bambu AMS/RFID catalog"),
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

    def test_check3c_unresolvable_instantiated_filament(self):
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


class TestCheck5(OfCleanTreeCase):
    def test_5a_empty_vendor_is_hard_error(self):
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
        # No grandfathering: sanctioning the tree does not silence check 5a.
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 1, out)
        self.assertIn("resolves empty filament_vendor", out)

    def test_5b_divergent_filament_triples(self):
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
        # No grandfathering: sanctioning the tree does not silence check 5b.
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 1, out)
        self.assertIn("divergent triples", out)

    def test_5_cross_bundle_divergence_is_warning_only(self):
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


class TestCheck6(OfCleanTreeCase):
    def _write_map(self, rows):
        path = os.path.join(self.t.dir, "bambu_filament_ids.json")
        ubfi.write_map(path, rows, "testcommit", "2026-09-04")
        return path

    def _write_raw_map(self, payload):
        """Write a map write_map() would never produce (hand-edited or mis-generated)."""
        path = os.path.join(self.t.dir, "bambu_filament_ids.json")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(payload, f, indent=2, ensure_ascii=False, sort_keys=True)
        return path

    def test_row_triple_must_match_tree(self):
        fid = afi.generate_filament_id("V", "PLA", "Foo")
        self.t.write_preset("VendorA", preset("Foo @base", filament_id=fid,
                                              instantiation=False,
                                              filament_vendor="V", filament_type="PLA"))
        self.t.write_preset("VendorA", preset("Foo @P1", inherits="Foo @base",
                                              compatible_printers=["P1"]))
        map_path = self._write_map(
            {fid: {"bambu_id": "GFZ00", "vendor": "V", "type": "PLA", "name": "Bar"}})
        errors, out = self.t.check(map_path)
        self.assertGreater(errors, 0)
        self.assertIn("regenerate the map", out)

    def test_non_of_map_key_is_an_error(self):
        # The map is keyed by OUR ids; a Bambu id in the key column means the
        # map was generated or hand-edited the wrong way round.
        map_path = self._write_map({
            "GFB00": {"bambu_id": "GFB00", "vendor": "V", "type": "PLA",
                      "name": "Foo"}})
        errors, out = self.t.check(map_path)
        self.assertEqual(errors, 1, out)
        self.assertIn('"GFB00" is not a minted "OF" id', out)

    def test_duplicate_bambu_id_is_an_error(self):
        map_path = self._write_map({
            "OFaaaaaa": {"bambu_id": "GFZ00", "vendor": "V", "type": "PLA", "name": "Foo"},
            "OFbbbbbb": {"bambu_id": "GFZ00", "vendor": "V", "type": "PETG", "name": "Bar"},
        })
        errors, out = self.t.check(map_path)
        self.assertGreater(errors, 0)
        self.assertIn("GFZ00", out)

    def test_row_for_unshipped_product_is_fine(self):
        map_path = self._write_map({
            "OFcccccc": {"bambu_id": "GFZ99", "vendor": "Nobody", "type": "PLA",
                        "name": "Ships Nothing"},
        })
        errors, out = self.t.check(map_path)
        self.assertEqual(errors, 0, out)

    def test_non_object_top_level_is_a_clean_error(self):
        # A hand-edited map that is a list (or any non-object) must report the map,
        # not raise AttributeError out of the check.
        map_path = self._write_raw_map([{"bambu_id": "GFZ00"}])
        errors, out = self.t.check(map_path)
        self.assertGreater(errors, 0)
        self.assertIn("does not parse", out)
        self.assertIn("regenerate the map", out)

    def test_empty_filaments_section_is_an_error(self):
        # What a regeneration against the wrong --bambustudio-dir writes: a well-formed
        # header with zero rows. At runtime every translation silently becomes identity.
        map_path = self._write_map({})
        errors, out = self.t.check(map_path)
        self.assertGreater(errors, 0)
        self.assertIn('no "filaments" rows', out)

    def test_absent_filaments_section_is_an_error(self):
        map_path = self._write_raw_map({
            "source": "https://github.com/bambulab/BambuStudio",
            "bambustudio_commit": "testcommit",
            "generated": "2026-09-04",
        })
        errors, out = self.t.check(map_path)
        self.assertGreater(errors, 0)
        self.assertIn('no "filaments" rows', out)

    def test_row_without_bambu_id_is_an_error(self):
        # Two such rows used to collide on None and be reported as a duplicate id.
        map_path = self._write_map({
            "OFaaaaaa": {"vendor": "V", "type": "PLA", "name": "Foo"},
            "OFbbbbbb": {"vendor": "V", "type": "PETG", "name": "Bar"},
        })
        errors, out = self.t.check(map_path)
        self.assertGreater(errors, 0)
        self.assertIn('"OFaaaaaa" declares no "bambu_id"', out)
        self.assertIn('"OFbbbbbb" declares no "bambu_id"', out)
        self.assertNotIn("mapped by both", out)

    def test_empty_bambu_id_is_an_error(self):
        # "" would land in the runtime map and translate an empty tray id into a filament.
        map_path = self._write_map({
            "OFaaaaaa": {"bambu_id": "", "vendor": "V", "type": "PLA", "name": "Foo"},
        })
        errors, out = self.t.check(map_path)
        self.assertGreater(errors, 0)
        self.assertIn('declares no "bambu_id"', out)


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
        self.assertEqual(list(snap), ["ids"])  # state only, no exception lists
        self.assertEqual(list(snap["ids"]), sorted(snap["ids"]))
        self.assertEqual(snap["ids"]["AX01"], {
            "filaments": ["VendorA/APLA"], "name": "APLA",
            "filament_type": "PLA", "filament_vendor": "AVendor"})
        self.assertEqual(snap["ids"]["OGFL99"], {
            "filaments": ["OrcaFilamentLibrary/Generic PLA", "VendorA/Generic PLA"],
            "name": "Generic PLA", "filament_type": "PLA", "filament_vendor": "Generic"})
        # Key order is part of the on-disk format.
        self.assertEqual(list(snap["ids"]["AX01"]),
                         ["filaments", "name", "filament_type", "filament_vendor"])

    def test_refuses_a_tree_it_could_not_read(self):
        # A bundle that does not parse contributes no ids, so sanctioning the
        # rest would record the loss as a deliberate removal.
        with open(self.t.snapshot, "rb") as f:
            before = f.read()
        with open(os.path.join(self.t.profiles, "VendorA",
                               "filament", "APLA @base.json"), "w",
                  encoding="utf-8") as f:
            f.write("{ not json")
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 1, out)
        self.assertIn("unreadable filament profile", out)
        with open(self.t.snapshot, "rb") as f:
            self.assertEqual(f.read(), before)

    def test_refuses_an_id_declared_under_two_triples(self):
        # VendorB re-declares APLA's id for a different product: one id, two
        # triples. No single entry can describe it, and check 3 rejects it anyway.
        self.t.add_vendor("VendorB", [
            preset("BPLA @base", filament_id="AX01", instantiation=False,
                   filament_vendor="BVendor", filament_type="PLA"),
            preset("BPLA @P1", inherits="BPLA @base", compatible_printers=["P1"]),
        ])
        with open(self.t.snapshot, "rb") as f:
            before = f.read()
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 1)
        self.assertIn('refusing to sanction filament_id "AX01": declared under 2 triples '
                      '(AVendor/PLA/APLA; BVendor/PLA/BPLA)', out)
        with open(self.t.snapshot, "rb") as f:
            self.assertEqual(f.read(), before)  # nothing written on refusal

    def test_refuses_reserved_namespace_ids(self):
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

    def test_dry_run_reports_without_writing(self):
        self.t.write_preset("VendorA", preset("ANEW @P2", inherits="APLA @base",
                                              compatible_printers=["P2"]))
        with open(self.t.snapshot, "rb") as f:
            before = f.read()
        rc, out = self.t.update_snapshot(dry_run=True)
        self.assertEqual(rc, 0)
        self.assertIn("would be rewritten", out)
        self.assertIn("claims added      : 1", out)
        with open(self.t.snapshot, "rb") as f:
            self.assertEqual(f.read(), before)
        # the real run writes exactly what the dry run reported
        rc, out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        self.assertIn("snapshot written", out)
        snap = load_json_file(self.t.snapshot)
        self.assertEqual(snap["ids"]["AX01"]["filaments"],
                         ["VendorA/ANEW", "VendorA/APLA"])


# ---------------------------------------------------------------------------
# --generate: one rule for inserts and rewrites alike
# ---------------------------------------------------------------------------

class TestAssign(OfCleanTreeCase):
    def test_noop_on_fully_idded_tree(self):
        changed, errors, out = self.t.assign()
        self.assertEqual((changed, errors), (0, 0))
        self.assertIn("filament_ids inserted  : 0", out)
        self.assertIn("filament_ids re-minted : 0", out)

    def test_mints_into_filament_root_and_rootless_member(self):
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
        self.assertIn("filament_ids inserted  : 0", out)

    def test_refuses_filament_with_incomplete_triple(self):
        self.t.write_preset("VendorA", preset("ENEW @base", instantiation=False))
        self.t.write_preset("VendorA", preset("ENEW @P1", inherits="ENEW @base",
                                              compatible_printers=["P1"]))
        changed, errors, out = self.t.assign()
        self.assertEqual(changed, 0)
        self.assertGreater(errors, 0)
        self.assertIn("resolves empty filament_vendor and filament_type", out)
        self.assertIn("mint key needs both", out)

    def test_refuses_to_re_mint_a_declaration_with_an_incomplete_triple(self):
        # The rewrite half of the same guard: a declared id that is not its
        # triple's mint still cannot be re-derived without a filament_vendor.
        self.t.write_preset("VendorA", preset("QNEW @base", filament_id="OFZZZZZZ",
                                              instantiation=False,
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("QNEW @P1", inherits="QNEW @base",
                                              compatible_printers=["P1"]))
        before = self.t.bytes_map()
        changed, errors, out = self.t.assign()
        self.assertEqual((changed, errors), (0, 1), out)
        self.assertIn("resolves empty filament_vendor", out)
        self.assertIn("mint key needs both", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_refuses_filament_with_divergent_root_fields(self):
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

    def test_parent_of_another_filament_never_receives_the_key(self):
        # Members whose id-less parent belongs to another filament (here one
        # parent shared by two filaments) carry the key themselves: the
        # parent's own triple would mint a different id (check 3).
        self.t.write_preset("VendorA", preset("shared_base", instantiation=False,
                                              filament_vendor="SV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("HNEW @P1", inherits="shared_base",
                                              compatible_printers=["P1"]))
        self.t.write_preset("VendorA", preset("INEW @P1", inherits="shared_base",
                                              compatible_printers=["P1"]))
        changed, errors, out = self.t.assign()
        self.assertEqual(errors, 0, out)
        self.assertEqual(changed, 2)
        for filament_name in ("HNEW", "INEW"):
            member = load_json_file(self.t.preset_path("VendorA", f"{filament_name} @P1"))
            self.assertEqual(member["filament_id"],
                             afi.generate_filament_id("SV", "PLA", filament_name))
        parent = load_json_file(self.t.preset_path("VendorA", "shared_base"))
        self.assertNotIn("filament_id", parent)
        # ... and the tree they leave behind passes the identity check.
        rc, _out = self.t.update_snapshot()
        self.assertEqual(rc, 0)
        errors, out = self.t.check()
        self.assertEqual(errors, 0, out)

    def test_non_of_declaration_is_re_minted(self):
        # A declaration that isn't OF-format (e.g. a vendor bundle synced from
        # an upstream catalog, like BBL's GF ids) is not the mint of its own
        # triple, so the same pass rewrites it in place.
        self.t.write_preset("VendorA", preset("Synced PLA @base", filament_id="GFZZ00",
                                              instantiation=False,
                                              filament_vendor="ZV", filament_type="PLA"))
        self.t.write_preset("VendorA", preset("Synced PLA @P1",
                                              inherits="Synced PLA @base",
                                              compatible_printers=["P1"]))
        changed, errors, out = self.t.assign()
        self.assertEqual(errors, 0, out)
        self.assertEqual(changed, 1)
        path = self.t.preset_path("VendorA", "Synced PLA @base")
        root = load_json_file(path)
        want = afi.generate_filament_id("ZV", "PLA", "Synced PLA")
        self.assertEqual(root["filament_id"], want)
        # the value was replaced in place, not appended as a second key
        with open(path, encoding="utf-8") as f:
            raw = f.read()
        self.assertEqual(raw.count('"filament_id"'), 1)
        # idempotent: the id is its triple's mint now, so a second run is a no-op
        changed, errors, _out = self.t.assign()
        self.assertEqual((changed, errors), (0, 0))

    def test_non_of_multi_root_filament_converges_on_one_id(self):
        # Two per-printer roots of one product (same triple), both carrying
        # the SAME non-OF id — the shape a synced vendor bundle ships (e.g.
        # BambuStudio's own per-printer @base files). They must converge on
        # one freshly minted id, not split into two.
        self.t.write_preset("VendorA", preset("Synced ABS @P1base", filament_id="GFSYNC0",
                                              instantiation=False,
                                              filament_vendor="ZV", filament_type="ABS"))
        self.t.write_preset("VendorA", preset("Synced ABS @P2base", filament_id="GFSYNC0",
                                              instantiation=False,
                                              filament_vendor="ZV", filament_type="ABS"))
        self.t.write_preset("VendorA", preset("Synced ABS @P1",
                                              inherits="Synced ABS @P1base",
                                              compatible_printers=["P1"]))
        self.t.write_preset("VendorA", preset("Synced ABS @P2",
                                              inherits="Synced ABS @P2base",
                                              compatible_printers=["P2"]))
        changed, errors, out = self.t.assign()
        self.assertEqual(errors, 0, out)
        self.assertEqual(changed, 2)
        want = afi.generate_filament_id("ZV", "ABS", "Synced ABS")
        b1 = load_json_file(self.t.preset_path("VendorA", "Synced ABS @P1base"))
        b2 = load_json_file(self.t.preset_path("VendorA", "Synced ABS @P2base"))
        self.assertEqual(b1["filament_id"], want)
        self.assertEqual(b2["filament_id"], want)

    def test_converges_on_an_existing_tree_id_for_the_same_triple(self):
        # An id-less filament whose product is already shipped (with its
        # conforming id) in another bundle converges on that id. One product,
        # one id, in every bundle.
        want = afi.generate_filament_id("CV", "PLA", "CPLA")
        self.t.add_vendor("VendorB", [
            preset("CPLA @base", filament_id=want, instantiation=False,
                   filament_vendor="CV", filament_type="PLA"),
            preset("CPLA @PB", inherits="CPLA @base",
                   compatible_printers=["PB 0.4 nozzle"]),
        ])
        self.t.write_preset("VendorA", preset("CPLA @base", instantiation=False,
                                              filament_vendor="CV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("CPLA @P1", inherits="CPLA @base",
                                              compatible_printers=["P1"]))
        changed, errors, out = self.t.assign()
        self.assertEqual((changed, errors), (1, 0), out)
        root = load_json_file(self.t.preset_path("VendorA", "CPLA @base"))
        self.assertEqual(root["filament_id"], want)

    def test_a_squatted_id_is_still_minted_for_its_own_product(self):
        # VendorB's "Other" declares the id that belongs to VendorA's "DPLA" —
        # a copy-paste, not a claim. The id is the mint of DPLA's triple and
        # nothing else, so DPLA gets it whatever VendorB carries.
        want0 = afi.generate_filament_id("DV", "PLA", "DPLA")
        self.t.add_vendor("VendorB", [
            preset("Other @base", filament_id=want0, instantiation=False,
                   filament_vendor="OV", filament_type="ABS"),
        ])
        self.t.write_preset("VendorA", preset("DPLA @base", instantiation=False,
                                              filament_vendor="DV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("DPLA @P1", inherits="DPLA @base",
                                              compatible_printers=["P1"]))
        changed, errors, out = self.t.assign(["VendorA"])
        self.assertEqual((changed, errors), (1, 0), out)
        root = load_json_file(self.t.preset_path("VendorA", "DPLA @base"))
        self.assertEqual(root["filament_id"], want0)
        # VendorB's declaration is VendorB's own mismatch: a run not allowed to
        # touch it leaves it to --check, and a run over VendorB re-mints it.
        _errors, out = self.t.check()
        self.assertIn('"Other @base" (VendorB/filament/Other @base.json) does not match '
                      'the mint of its triple', out)
        changed, errors, out = self.t.assign(["VendorB"])
        self.assertEqual((changed, errors), (1, 0), out)
        other = load_json_file(self.t.preset_path("VendorB", "Other @base"))
        self.assertEqual(other["filament_id"],
                         afi.generate_filament_id("OV", "ABS", "Other"))

    def test_generate_refuses_to_write_into_a_collision(self):
        # "X" ships on the id its triple mints; a new product "Y" whose triple
        # mints the very same id is a base62 collision. The run does not salt
        # past it: it reports both products, writes nothing for "Y", and leaves
        # the remedy — a rename, so the triples differ — to the author.
        collide = {("V", "PLA", "X"), ("W", "ABS", "Y")}
        real = afi.generate_filament_id

        def colliding(vendor, ftype, name):
            return "OFcolid0" if (vendor, ftype, name) in collide else real(vendor, ftype, name)

        afi.generate_filament_id = colliding
        self.addCleanup(setattr, afi, "generate_filament_id", real)
        self.t.write_preset("VendorA", preset("X @base", filament_id="OFcolid0",
                                              instantiation=False,
                                              filament_vendor="V", filament_type="PLA"))
        self.t.write_preset("VendorA", preset("X @P1", inherits="X @base",
                                              compatible_printers=["P1"]))
        self.t.write_preset("VendorA", preset("Y @base", instantiation=False,
                                              filament_vendor="W", filament_type="ABS"))
        self.t.write_preset("VendorA", preset("Y @P1", inherits="Y @base",
                                              compatible_printers=["P1"]))
        before = self.t.bytes_map()
        changed, errors, out = self.t.assign()
        self.assertEqual((changed, errors), (0, 1), out)
        self.assertIn("V/PLA/X", out)
        self.assertIn("W/ABS/Y", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_mismatching_of_declaration_is_re_derived(self):
        # One rule: an id that is not the mint of its own triple is rewritten.
        self.t.write_preset("VendorA", preset("KNEW @base", filament_id="OFZZZZZZ",
                                              instantiation=False,
                                              filament_vendor="KV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("KNEW @P1", inherits="KNEW @base",
                                              compatible_printers=["P1"]))
        want = afi.generate_filament_id("KV", "PLA", "KNEW")
        changed, errors, out = self.t.assign()
        self.assertEqual((changed, errors), (1, 0), out)
        self.assertIn('"OFZZZZZZ" -> "%s"' % want, out)
        root = load_json_file(self.t.preset_path("VendorA", "KNEW @base"))
        self.assertEqual(root["filament_id"], want)
        changed, errors, _out = self.t.assign()
        self.assertEqual((changed, errors), (0, 0))

    def test_vendor_filter_limits_rewrites_and_inserts_alike(self):
        # A mismatching declarer in VendorA and an id-less filament in VendorB;
        # only VendorA is written, and VendorB's bytes are untouched.
        self.t.write_preset("VendorA", preset("LNEW @base", filament_id="OFZZZZZZ",
                                              instantiation=False,
                                              filament_vendor="LV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("LNEW @P1", inherits="LNEW @base",
                                              compatible_printers=["P1"]))
        self.t.add_vendor("VendorB", [
            preset("MNEW @base", instantiation=False,
                   filament_vendor="MV", filament_type="PLA"),
            preset("MNEW @PB", inherits="MNEW @base",
                   compatible_printers=["PB 0.4 nozzle"]),
        ])
        before = self.t.bytes_map()
        changed, errors, out = self.t.assign(["VendorA"])
        self.assertEqual((changed, errors), (1, 0), out)
        root = load_json_file(self.t.preset_path("VendorA", "LNEW @base"))
        self.assertEqual(root["filament_id"],
                         afi.generate_filament_id("LV", "PLA", "LNEW"))
        after = self.t.bytes_map()
        for rel, raw in before.items():
            if rel.split(os.sep)[0].startswith("VendorB"):
                self.assertEqual(after[rel], raw, rel)
        # ... and the deferred half is exactly what a VendorB run then writes
        changed, errors, out = self.t.assign(["VendorB"])
        self.assertEqual((changed, errors), (1, 0), out)
        b_root = load_json_file(self.t.preset_path("VendorB", "MNEW @base"))
        self.assertEqual(b_root["filament_id"],
                         afi.generate_filament_id("MV", "PLA", "MNEW"))

    def test_unknown_vendor_reports_and_writes_nothing(self):
        # A real insert is pending, so "wrote nothing" means the unknown vendor
        # aborted the run before any write — not that the tree was already done.
        self.t.write_preset("VendorA", preset("PNEW @base", instantiation=False,
                                              filament_vendor="PV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("PNEW @P1", inherits="PNEW @base",
                                              compatible_printers=["P1"]))
        before = self.t.bytes_map()
        changed, errors, out = self.t.assign(["Nope"])
        self.assertEqual((changed, errors), (0, 1))
        self.assertIn("unknown vendor", out)
        self.assertEqual(self.t.bytes_map(), before)
        # ... and that pending insert is real: a known-vendor run makes it.
        changed, errors, out = self.t.assign(["VendorA"])
        self.assertEqual((changed, errors), (1, 0), out)
        self.assertEqual(
            load_json_file(self.t.preset_path("VendorA", "PNEW @base"))["filament_id"],
            afi.generate_filament_id("PV", "PLA", "PNEW"))

    def test_dry_run_reports_the_real_run_and_writes_nothing(self):
        self.t.write_preset("VendorA", preset("NNEW @base", instantiation=False,
                                              filament_vendor="NV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset("NNEW @P1", inherits="NNEW @base",
                                              compatible_printers=["P1"]))
        before = self.t.bytes_map()
        dry_changed, errors, out = self.t.assign(dry_run=True)
        self.assertEqual((dry_changed, errors), (1, 0), out)
        self.assertIn("would insert", out)
        self.assertEqual(self.t.bytes_map(), before)
        changed, errors, out = self.t.assign()
        self.assertEqual((changed, errors), (dry_changed, 0), out)
        root = load_json_file(self.t.preset_path("VendorA", "NNEW @base"))
        self.assertEqual(root["filament_id"],
                         afi.generate_filament_id("NV", "PLA", "NNEW"))


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
        text, n = afi.delete_key_line(self.CRLF_WITH_ID, "filament_id", "OFold123")
        self.assertEqual(n, 1)
        self.assertNotIn("filament_id", json.loads(text))
        self.assertEqual(text, self.CRLF_WITH_ID.replace(
            '\t"filament_id": "OFold123",\r\n', "", 1))

    def test_delete_last_property_line(self):
        lf_text = ('{\n    "name": "K @base",\n    "instantiation": "false",\n'
                   '    "filament_id": "OFold123"\n}\n')
        text, n = afi.delete_key_line(lf_text, "filament_id", "OFold123")
        self.assertEqual(n, 1)
        data = json.loads(text)
        self.assertNotIn("filament_id", data)
        self.assertEqual(data["instantiation"], "false")
        # the preceding comma goes with it, and nothing else moves
        self.assertEqual(text, '{\n    "name": "K @base",\n'
                               '    "instantiation": "false"\n}\n')

    def test_dry_run_edits_verify_but_write_nothing(self):
        t = SyntheticTree()
        self.addCleanup(t.cleanup)
        t.add_vendor("VendorA", [])
        fresh = t.preset_path("VendorA", "JNEW @base")
        with open(fresh, "wb") as f:
            f.write(self.CRLF_TEXT.encode("utf-8"))
        idded = t.preset_path("VendorA", "JOLD @base")
        with open(idded, "wb") as f:
            f.write(self.CRLF_WITH_ID.encode("utf-8"))

        afi.write_filament_id(fresh, "OFabc123", dry_run=True)
        afi.rewrite_filament_id(idded, "OFold123", "OFnew456", dry_run=True)
        with open(fresh, "rb") as f:
            self.assertEqual(f.read(), self.CRLF_TEXT.encode("utf-8"))
        with open(idded, "rb") as f:
            self.assertEqual(f.read(), self.CRLF_WITH_ID.encode("utf-8"))
        # the edit is still applied and verified in memory: only the write is
        # skipped, so a stale old id fails just as loudly
        with self.assertRaises(RuntimeError):
            afi.rewrite_filament_id(idded, "OFstale1", "OFnew456", dry_run=True)


# ---------------------------------------------------------------------------
# --generate: re-minting non-conformant declarations
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
        ofl_root = load_json_file(self.t.preset_path(OFL, "Generic PLA @base"))
        self.assertEqual(ofl_root["filament_id"], "OGFL99")
        # idempotent: a second run over the same vendor rewrites nothing
        changed, errors, _out = self.t.remint(["VendorA"])
        self.assertEqual((changed, errors), (0, 0))

    def test_second_id_of_one_product_is_converged_by_remint(self):
        # A second preset of one product kept on an id of its own is not a
        # sanctioned split: the triple determines the id, so --generate pulls it
        # back. An AMS ambiguity this exposes is fixed in the profiles instead.
        want = afi.generate_filament_id(*self.TRIPLE)
        self.t.write_preset("VendorA", preset("APLA @legacy", filament_id="OFlegac1",
                                              inherits="APLA @base",
                                              compatible_printers=["P2 0.4 nozzle"]))
        changed, errors, out = self.t.remint(["VendorA"])
        self.assertEqual(errors, 0, out)
        self.assertEqual(changed, 2)  # the non-conformant AX01 root and @legacy
        legacy = load_json_file(self.t.preset_path("VendorA", "APLA @legacy"))
        self.assertEqual(legacy["filament_id"], want)

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

    def test_not_blocked_by_a_non_conformant_occurrence(self):
        # VendorB carries APLA's id under a PETG triple of its own: that
        # declaration is wrong, and it costs VendorA's APLA nothing — the id is
        # the mint of APLA's triple, whoever else is squatting on it.
        want0 = afi.generate_filament_id(*self.TRIPLE)
        self.t.add_vendor("VendorB", [
            preset("BPLA @base", filament_id=want0, instantiation=False,
                   filament_vendor="BV", filament_type="PETG"),
        ])
        changed, errors, out = self.t.remint(["VendorA"])
        self.assertEqual((changed, errors), (1, 0), out)
        root = load_json_file(self.t.preset_path("VendorA", "APLA @base"))
        self.assertEqual(root["filament_id"], want0)
        # A run over VendorB re-mints the squatter to its own triple.
        changed, errors, out = self.t.remint(["VendorB"])
        self.assertEqual((changed, errors), (1, 0), out)
        self.assertEqual(
            load_json_file(self.t.preset_path("VendorB", "BPLA @base"))["filament_id"],
            afi.generate_filament_id("BV", "PETG", "BPLA"))

    def test_bbl_is_reminted_like_any_vendor(self):
        self.t.add_vendor("BBL", [
            preset("Bambu ABS @base", filament_id="GFB00", instantiation=False,
                   filament_vendor="Bambu Lab", filament_type="ABS"),
            preset("Bambu ABS @P1", inherits="Bambu ABS @base",
                   compatible_printers=["P1"]),
        ])
        changed, errors, out = self.t.remint(["BBL"])
        self.assertEqual(errors, 0, out)
        self.assertEqual(changed, 1)
        root = load_json_file(self.t.preset_path("BBL", "Bambu ABS @base"))
        self.assertEqual(root["filament_id"],
                         afi.generate_filament_id("Bambu Lab", "ABS", "Bambu ABS"))

    def test_dry_run_leaves_every_file_untouched(self):
        before = self.t.bytes_map()
        changed, errors, out = self.t.remint(["VendorA"], dry_run=True)
        self.assertEqual((changed, errors), (1, 0), out)
        self.assertIn("would rewrite", out)
        self.assertEqual(self.t.bytes_map(), before)
        # the real run then makes exactly that one change
        changed, errors, _out = self.t.remint(["VendorA"])
        self.assertEqual((changed, errors), (1, 0))
        root = load_json_file(self.t.preset_path("VendorA", "APLA @base"))
        self.assertEqual(root["filament_id"],
                         afi.generate_filament_id(*self.TRIPLE))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

class TestCli(unittest.TestCase):
    """main(argv) over a synthetic tree. The clean tree's baseline ids are
    deliberately non-conformant ("AX01"/"OGFL99"), so a --generate run always
    has both a filament_id rewrite and setting_id inserts to do."""

    def setUp(self):
        self.t = make_clean_tree()
        self.addCleanup(self.t.cleanup)

    def test_bare_invocation_prints_help(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = afi.main([])
        self.assertEqual(rc, 0)
        self.assertIn("usage:", buf.getvalue())
        self.assertIn("--generate", buf.getvalue())
        # ... and so does any invocation naming no mode: help, and no work
        before = self.t.bytes_map()
        rc, out = self.t.cli()
        self.assertEqual(rc, 0)
        self.assertIn("usage:", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_another_tree_needs_its_own_snapshot(self):
        # --profiles retargets the tree, but the sanctioned state of that tree
        # is not the repo snapshot: checking against it is meaningless and
        # re-recording into it would overwrite the tracked file.
        with open(afi.SNAPSHOT_PATH, "rb") as f:
            repo_snapshot = f.read()
        for mode in ("--check", "--update-snapshot"):
            with self.assertRaises(SystemExit) as caught:
                with contextlib.redirect_stderr(io.StringIO()):
                    afi.main([mode, "--profiles", self.t.profiles])
            self.assertEqual(caught.exception.code, 2, mode)
        with open(afi.SNAPSHOT_PATH, "rb") as f:
            self.assertEqual(f.read(), repo_snapshot)
        # Named explicitly, both modes run against that tree.
        rc, out = self.t.cli("--update-snapshot")
        self.assertEqual(rc, 0, out)
        # --generate never reads the snapshot, so it keeps working without one.
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = afi.main(["--dry-run", "--profiles", self.t.profiles])
        self.assertEqual(rc, 0, buf.getvalue())

    def test_filament_id_and_setting_id_together_are_rejected(self):
        # Each flag's help promises it skips the other kind, so the pair cannot
        # quietly mean "both".
        with self.assertRaises(SystemExit) as caught:
            with contextlib.redirect_stderr(io.StringIO()):
                afi.main(["--generate", "--filament-id", "--setting-id",
                          "--profiles", self.t.profiles])
        self.assertEqual(caught.exception.code, 2)

    def test_a_run_with_errors_does_not_report_success(self):
        # An unreadable profile must not be buried under a green summary line.
        path = self.t.preset_path("VendorA", "APLA @base")
        with open(path, "w", encoding="utf-8") as f:
            f.write("{ not json")
        rc, out = self.t.cli("--generate")
        self.assertEqual(rc, 1, out)
        self.assertIn("error(s)", out)
        self.assertNotIn("SUCCESS", out)

    def test_dry_run_alone_previews_generate(self):
        before = self.t.bytes_map()
        rc, out = self.t.cli("--dry-run")
        self.assertEqual(rc, 0, out)
        self.assertIn("would ", out)
        self.assertIn("nothing written", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_the_reported_count_is_files_not_edits(self):
        # A file both passes touch is still one file. "SoloPLA @P1" needs a
        # filament_id rewrite AND a setting_id insert, so an edit-counting
        # summary would over-report the tree.
        self.t.write_preset("VendorA", preset(
            "SoloPLA @P1", filament_id="AX01", filament_vendor="AVendor",
            filament_type="PLA", compatible_printers=["P1"]))
        before = self.t.bytes_map()

        rc, out = self.t.cli("--generate")

        self.assertEqual(rc, 0, out)
        after = self.t.bytes_map()
        touched = {p for p in after if after[p] != before.get(p)}
        self.assertIn("VendorA/filament/SoloPLA @P1.json".replace("/", os.sep), touched)
        self.assertIn("rewrite VendorA/filament/SoloPLA @P1.json", out)
        self.assertIn("update VendorA/filament/SoloPLA @P1.json", out)
        summary = [l for l in out.splitlines() if "file(s) changed" in l]
        self.assertEqual(len(summary), 1, out)
        self.assertIn(f"{len(touched)} file(s) changed", summary[0])

    def test_dryrun_is_the_same_flag(self):
        before = self.t.bytes_map()
        rc, out = self.t.cli("--dryrun")
        self.assertEqual(rc, 0, out)
        self.assertIn("would ", out)  # the same preview, not a silent no-op
        self.assertIn("nothing written", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_generate_vendor_writes_only_in_that_bundle(self):
        before = self.t.bytes_map()
        rc, out = self.t.cli("--generate", "--vendor", "VendorA")
        self.assertEqual(rc, 0, out)
        after = self.t.bytes_map()
        changed = sorted(rel for rel in before if after[rel] != before[rel])
        self.assertTrue(changed, out)
        for rel in changed:
            self.assertTrue(rel.startswith("VendorA" + os.sep), rel)
        # The bundles it spared were not simply already conformant: the
        # un-narrowed run goes on to write in them too.
        rc, out = self.t.cli("--generate")
        self.assertEqual(rc, 0, out)
        final = self.t.bytes_map()
        self.assertTrue(any(final[rel] != after[rel] for rel in after
                            if not rel.startswith("VendorA" + os.sep)), out)

    def test_generate_unknown_vendor_returns_1(self):
        before = self.t.bytes_map()
        rc, out = self.t.cli("--generate", "--vendor", "Nope")
        self.assertEqual(rc, 1)
        self.assertIn("unknown vendor", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_setting_id_only_leaves_filament_ids_alone(self):
        rc, out = self.t.cli("--generate", "--setting-id")
        self.assertEqual(rc, 0, out)
        root = load_json_file(self.t.preset_path("VendorA", "APLA @base"))
        self.assertEqual(root["filament_id"], "AX01")  # not re-minted
        member = load_json_file(self.t.preset_path("VendorA", "APLA @P1"))
        self.assertNotIn("filament_id", member)
        self.assertEqual(member["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "filament",
                                                        "APLA @P1"))

    def test_filament_id_only_inserts_no_setting_id(self):
        rc, out = self.t.cli("--generate", "--filament-id")
        self.assertEqual(rc, 0, out)
        root = load_json_file(self.t.preset_path("VendorA", "APLA @base"))
        self.assertEqual(root["filament_id"],
                         afi.generate_filament_id("AVendor", "PLA", "APLA"))
        for name in ("APLA @base", "APLA @P1"):
            self.assertNotIn(
                "setting_id", load_json_file(self.t.preset_path("VendorA", name)))

    def test_check_mode_returns_1_on_errors(self):
        # What CI keys off: --check exits nonzero when the tree does not match
        # the snapshot it is validated against.
        before = self.t.bytes_map()
        rc, out = self.t.cli("--check")
        self.assertEqual(rc, 1)
        self.assertIn("error(s)", out)
        self.assertEqual(self.t.bytes_map(), before)  # --check never writes

    def test_removed_and_conflicting_flags_are_rejected(self):
        for argv in (["--remint", "VendorA"],          # removed mode
                     ["--mint", "A/B/C"],              # removed mode
                     ["--drop-redundant-ids", "VendorA"],  # removed mode
                     ["--assign"],                     # removed mode
                     ["--generate", "--check"],        # two modes
                     ["--vendor", "VendorA"],          # narrowing without a mode
                     ["--filament-id"],                # narrowing without a mode
                     ["--check", "--vendor", "VendorA"]):  # narrowing on --check
            with self.subTest(argv=argv):
                with self.assertRaises(SystemExit) as cm, \
                        contextlib.redirect_stdout(io.StringIO()), \
                        contextlib.redirect_stderr(io.StringIO()):
                    afi.main([*argv, "--profiles", self.t.profiles])
                self.assertEqual(cm.exception.code, 2)


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

    def test_check_cli_returns_0(self):
        # The exact CI invocation, return code included.
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = afi.main(["--check"])
        self.assertEqual(rc, 0, buf.getvalue())

    def test_every_instantiated_filament_resolves_an_id(self):
        analysis = afi.analyze_tree(REAL_PROFILES)
        self.assertEqual(analysis["missing_effective"], [])
        self.assertEqual(analysis["read_errors"], [])

# ---------------------------------------------------------------------------
# review-fix regressions
# ---------------------------------------------------------------------------

class TestReviewFixes(OfCleanTreeCase):
    def test_one_file_reached_by_two_spellings_counts_once(self):
        # The filament pass reaches a file through its index sub_path, the
        # setting_id pass through os.walk. Those two spellings differ whenever
        # the sub_path is not already normalized - always, on Windows, where
        # every sub_path keeps the "/" the index stores. One file is one file.
        # One preset per write path: SoloPLA has no id (inserted), WrongPLA
        # declares one that is not its own mint (rewritten). Both are
        # instantiated, so the setting_id pass reaches them too.
        self.t.write_preset("VendorA", preset("SoloPLA @P1",
                                              compatible_printers=["P1 0.4 nozzle"],
                                              filament_vendor="SV",
                                              filament_type="PLA"))
        self.t.write_preset("VendorA", preset(
            "WrongPLA @P1", filament_id=afi.generate_filament_id("WV", "PLA", "Nope"),
            compatible_printers=["P1 0.4 nozzle"],
            filament_vendor="WV", filament_type="PLA"))
        for name in ("SoloPLA @P1", "WrongPLA @P1"):
            self.t.set_sub_path("VendorA", name, f"filament/./{name}.json")
        touched = set()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            afi.generate_filament_ids(self.t.profiles, None, False, touched)
            afi.generate_setting_ids(self.t.profiles, None, False, touched)
        for name in ("SoloPLA @P1", "WrongPLA @P1"):
            self.assertEqual([t for t in touched if t.endswith(name + ".json")],
                             [self.t.preset_path("VendorA", name)], sorted(touched))
        self.assertEqual(sorted(touched),
                         sorted(os.path.normpath(t) for t in touched))

    def test_a_broken_edit_names_the_file(self):
        # An edit that produces invalid JSON is refused, and the message has to
        # say which of ~12,000 files it was: a bare JSONDecodeError does not.
        path = self.t.preset_path("VendorA", "APLA @base")
        with self.assertRaises(RuntimeError) as caught:
            afi._edit_profile(path, lambda text: (text.replace("{", "{,", 1), 1),
                              what="test edit")
        self.assertIn(path, str(caught.exception))
        self.assertIn("test edit", str(caught.exception))

    def test_check3_skips_of_id_inherited_from_other_vendor(self):
        # An OFL filament carries its own minted OF id and a vendor tunes it
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

    def test_check3c_prints_expected_mint(self):
        self.t.write_preset("VendorA", preset("Orphan PLA @P1",
                                              compatible_printers=["P1 0.4 nozzle"],
                                              filament_vendor="OV",
                                              filament_type="PLA"))
        errors, out = self.t.check()
        self.assertGreater(errors, 0)
        self.assertIn(afi.generate_filament_id("OV", "PLA", "Orphan PLA"), out)


# ---------------------------------------------------------------------------
# scripts/update_bambu_filament_ids.py: the generated Bambu catalog id map
# ---------------------------------------------------------------------------

class TestBambuMap(unittest.TestCase):
    def _bs_tree(self, filaments):
        # filaments: list of (name, bambu_id, vendor, type); builds a minimal BBL
        # bundle with an @base per filament carrying the id and one instantiated child.
        t = SyntheticTree()
        self.addCleanup(t.cleanup)
        presets = []
        for filament_name, bambu_id, vendor, ftype in filaments:
            presets.append(preset(f"{filament_name} @base", filament_id=bambu_id,
                                  instantiation=False, filament_vendor=vendor,
                                  filament_type=ftype))
            presets.append(preset(f"{filament_name} @P1", inherits=f"{filament_name} @base",
                                  compatible_printers=["P1 0.4 nozzle"]))
        t.add_vendor("BBL", presets)
        filaments, errors = afi.load_vendor_filaments(t.profiles, "BBL")
        self.assertEqual(errors, [])
        return filaments

    def test_one_row_per_filament(self):
        rows = ubfi.derive_rows(self._bs_tree([("Bambu ABS", "GFB00", "Bambu Lab", "ABS")]))
        self.assertEqual(rows, {afi.generate_filament_id("Bambu Lab", "ABS", "Bambu ABS"):
                                {"bambu_id": "GFB00", "vendor": "Bambu Lab", "type": "ABS", "name": "Bambu ABS"}})

    def test_shared_bambu_id_is_an_error(self):
        with self.assertRaises(SystemExit):
            ubfi.derive_rows(self._bs_tree([("A", "GFX00", "V", "PLA"), ("B", "GFX00", "V", "PLA")]))

    @unittest.skipUnless(os.path.isdir("/Users/lijiang/codes/BambuStudio/resources/profiles"), "no local clone")
    def test_local_clone_yields_the_catalog(self):
        rows = ubfi.derive_rows(afi.load_vendor_filaments("/Users/lijiang/codes/BambuStudio/resources/profiles", "BBL")[0])
        self.assertEqual(len(rows), 100)
        self.assertEqual(len({r["bambu_id"] for r in rows.values()}), 100)


class TestWriteMap(unittest.TestCase):
    def test_format(self):
        d = tempfile.mkdtemp(prefix="bambu_map_test_")
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        # nested, not-yet-existing directory: write_map must create it
        path = os.path.join(d, "out", "bambu_filament_ids.json")
        rows = {"OFabc123": {"bambu_id": "GFB00", "vendor": "Bambu Lab", "type": "ABS", "name": "Bambu ABS"}}

        ubfi.write_map(path, rows, "66e405477", "2026-09-04")

        with open(path, "rb") as f:
            raw = f.read()
        self.assertTrue(raw.endswith(b"\n"))
        self.assertNotIn(b"\r", raw)
        text = raw.decode("utf-8")
        self.assertEqual(json.loads(text), {
            "source": "https://github.com/bambulab/BambuStudio",
            "bambustudio_commit": "66e405477",
            "generated": "2026-09-04",
            "filaments": rows,
        })
        # sorted top-level keys
        self.assertLess(text.index('"bambustudio_commit"'), text.index('"filaments"'))
        self.assertLess(text.index('"filaments"'), text.index('"generated"'))
        self.assertLess(text.index('"generated"'), text.index('"source"'))


class TestDriftReport(unittest.TestCase):
    def test_reports_both_directions(self):
        t = SyntheticTree()
        self.addCleanup(t.cleanup)
        t.add_vendor("BBL", [
            preset("Match PLA @base", filament_id="GFX01", instantiation=False,
                  filament_vendor="V", filament_type="PLA"),
            preset("Match PLA @P1", inherits="Match PLA @base",
                  compatible_printers=["P1"]),
            preset("Orphan PLA @base", filament_id="GFX03", instantiation=False,
                  filament_vendor="V", filament_type="PLA"),
            preset("Orphan PLA @P1", inherits="Orphan PLA @base",
                  compatible_printers=["P1"]),
        ])
        rows = {
            # matches the BBL bundle's "Match PLA" filament: no drift either way
            "OFmatch01": {"bambu_id": "GFX01", "vendor": "V", "type": "PLA", "name": "Match PLA"},
            # no filament of this identity in the BBL bundle above
            "OFghost01": {"bambu_id": "GFX02", "vendor": "V", "type": "PLA", "name": "Upstream Only PLA"},
        }
        orca_analysis = afi.analyze_tree(t.profiles)

        lines = ubfi.drift_report(rows, orca_analysis)

        report = "\n".join(lines)
        self.assertIn("Upstream Only PLA", report)
        self.assertIn("Orca BBL filaments with no row: 1", report)
        self.assertNotIn("Match PLA", report)  # the matched filament is not drift


if __name__ == "__main__":
    unittest.main()
