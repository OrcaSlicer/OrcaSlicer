#!/usr/bin/env python3
"""Tests for the setting_id half of scripts/orca_id_tool.py (stdlib unittest, no
external deps).

Run from the repo root:  python -m unittest discover -s scripts/tests -v
"""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import orca_id_tool as afi  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REAL_PROFILES = os.path.join(REPO_ROOT, "resources", "profiles")


# ---------------------------------------------------------------------------
# helpers: synthetic profile trees
# ---------------------------------------------------------------------------

def preset(name, instantiation=True, setting_id=None, settings_id=None,
           filament_id=None, type_name="filament", **extra):
    """A preset in the canonical key order the shipped profiles use."""
    data = {"type": type_name, "name": name, "from": "system"}
    if setting_id is not None:
        data["setting_id"] = setting_id
    if settings_id is not None:
        data["settings_id"] = settings_id
    if filament_id is not None:
        data["filament_id"] = filament_id
    data["instantiation"] = "true" if instantiation else "false"
    data.update(extra)
    return data


class SettingTree:
    """A throwaway resources/profiles-shaped directory of setting_id-bearing bundles.

    A bundle is a `<vendor>/` directory plus the sibling `<vendor>.json` index
    that makes list_vendor_names() see it; presets live under filament/,
    process/ and machine/, the three subdirs generate_setting_ids walks.

    write() also registers filament presets in the index's filament_list, the
    way a shipped bundle does. generate_setting_ids never reads that list, but
    generate_filament_ids does: without it the filament half of the tool is a
    no-op on this tree, and the tests that assert --setting-id leaves
    filament_ids alone would hold for the wrong reason.
    """

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="setting_id_test_")
        self.profiles = os.path.join(self.dir, "profiles")
        os.makedirs(self.profiles)

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def index_path(self, vendor):
        return os.path.join(self.profiles, vendor + ".json")

    def add_vendor(self, vendor):
        """Create the bundle dir and its index; idempotent, keeps the list."""
        for sub in afi.PROFILE_SUBDIRS:
            os.makedirs(os.path.join(self.profiles, vendor, sub), exist_ok=True)
        if not os.path.exists(self.index_path(vendor)):
            self._write_index(vendor, {"name": vendor, "version": "01.00.00.00",
                                       "filament_list": []})

    def _write_index(self, vendor, index):
        with open(self.index_path(vendor), "w", encoding="utf-8",
                  newline="\n") as f:
            json.dump(index, f, indent=4, ensure_ascii=False)

    def register(self, vendor, subdir, name):
        """Add a filament preset to the bundle index's filament_list."""
        with open(self.index_path(vendor), encoding="utf-8") as f:
            index = json.load(f)
        sub_path = os.path.join(subdir, name + ".json").replace(os.sep, "/")
        index["filament_list"].append({"name": name, "sub_path": sub_path})
        self._write_index(vendor, index)

    def path(self, vendor, subdir, name):
        return os.path.join(self.profiles, vendor, subdir, name + ".json")

    def write(self, vendor, subdir, data, name=None):
        """Write a preset as indented LF JSON; returns its path."""
        self.add_vendor(vendor)
        file_name = name if name is not None else data["name"]
        path = self.path(vendor, subdir, file_name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, indent=4, ensure_ascii=False)
            f.write("\n")
        if subdir.split(os.sep)[0] == "filament" and data.get("name"):
            self.register(vendor, subdir, file_name)
        return path

    def write_raw(self, vendor, subdir, name, raw):
        """Write exact bytes (BOM, CRLF, tabs, broken JSON); returns its path."""
        self.add_vendor(vendor)
        path = self.path(vendor, subdir, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(raw)
        return path

    def read(self, vendor, subdir, name):
        with open(self.path(vendor, subdir, name), encoding="utf-8-sig") as f:
            return json.load(f)

    def raw(self, vendor, subdir, name):
        with open(self.path(vendor, subdir, name), "rb") as f:
            return f.read()

    def bytes_map(self):
        """relative path -> bytes, for every file in the tree."""
        out = {}
        for root, dirs, files in os.walk(self.profiles):
            dirs.sort()
            for name in sorted(files):
                path = os.path.join(root, name)
                with open(path, "rb") as f:
                    out[os.path.relpath(path, self.profiles)] = f.read()
        return out

    # -- pipeline wrappers ---------------------------------------------------

    def run(self, vendors=None, dry_run=False):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            changed, errors = afi.generate_setting_ids(self.profiles, vendors, dry_run)
        return changed, errors, buf.getvalue()

    def run_filament_ids(self, vendors=None, dry_run=False):
        """The OTHER half of --generate."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            changed, errors = afi.generate_filament_ids(self.profiles, vendors, dry_run)
        return changed, errors, buf.getvalue()


class SettingTreeCase(unittest.TestCase):
    def setUp(self):
        self.t = SettingTree()
        self.addCleanup(self.t.cleanup)


# ---------------------------------------------------------------------------
# mint: the C++/Python byte-identity contract
# ---------------------------------------------------------------------------

class TestSettingIdMint(unittest.TestCase):
    # Copied verbatim from tests/libslic3r/test_preset_setting_id.cpp: the C++
    # generate_preset_setting_id() recomputes these ids on the fly, so the two
    # implementations must stay byte-identical.
    GOLDEN = [
        ("Afinia",   "filament", "Afinia ABS @Afinia H400",             "TL34qSVkppBvMvgH"),
        ("Afinia",   "process",  "0.20mm Standard @Afinia H400",        "FzmtNsy7XQvpd7w0"),
        ("Afinia",   "machine",  "Afinia H400 0.4 nozzle",              "r4FZagW0S8uoaJPd"),
        ("Anycubic", "filament", "Generic PLA @Anycubic Kobra 2",       "YIWGGLQ8Oepd30Fv"),
        ("Creality", "process",  "0.16mm Optimal @Creality Ender-3 V3", "2Nrbq8PxssUPBLza"),
        ("Elegoo",   "machine",  "Elegoo Neptune 4 0.4 nozzle",         "69QdWuRQwAZk9rFu"),
    ]

    def test_golden_vectors(self):
        for vendor, type_name, name, expected in self.GOLDEN:
            with self.subTest(name=name):
                self.assertEqual(
                    afi.generate_preset_setting_id(vendor, type_name, name), expected)

    def test_namespace_and_length_are_frozen(self):
        # Baked into the C++ side and into every shipped profile; never change it.
        self.assertEqual(afi.NAMESPACE,
                         uuid.UUID("c1f4d9e2-7a3b-5c8d-9e0f-1a2b3c4d5e6f"))
        self.assertEqual(afi.SETTING_ID_LENGTH, 16)
        self.assertEqual(
            afi.ALPHABET,
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz")
        self.assertEqual(len(afi.ALPHABET), 62)

    def test_format_is_sixteen_alphabet_chars(self):
        for vendor, type_name, name in [("Creality", "filament", "CR PLA @Ender"),
                                        ("Elegoo", "machine", "Elegoo Neptune 4"),
                                        ("拓竹", "filament", "拓竹 PLA @P1")]:
            with self.subTest(name=name):
                sid = afi.generate_preset_setting_id(vendor, type_name, name)
                self.assertEqual(len(sid), afi.SETTING_ID_LENGTH)
                self.assertTrue(set(sid) <= set(afi.ALPHABET), sid)

    def test_is_the_low_base62_digits_of_the_uuid5(self):
        # Independent re-implementation of the whole rule, key layout included.
        for vendor, type_name, name, _expected in self.GOLDEN:
            u = uuid.uuid5(afi.NAMESPACE, f"{vendor}/{type_name}/{name}")
            n = int.from_bytes(u.bytes, "big")
            digits = ""
            for _ in range(afi.SETTING_ID_LENGTH):
                digits = afi.ALPHABET[n % 62] + digits
                n //= 62
            self.assertEqual(afi.generate_preset_setting_id(vendor, type_name, name),
                             digits)

    def test_deterministic(self):
        a = afi.generate_preset_setting_id("VendorX", "filament", "My PLA")
        self.assertEqual(a, afi.generate_preset_setting_id("VendorX", "filament", "My PLA"))

    def test_every_identity_component_changes_the_id(self):
        base = afi.generate_preset_setting_id("VendorX", "filament", "My PLA")
        self.assertNotEqual(base, afi.generate_preset_setting_id("VendorY", "filament", "My PLA"))
        self.assertNotEqual(base, afi.generate_preset_setting_id("VendorX", "process", "My PLA"))
        self.assertNotEqual(base, afi.generate_preset_setting_id("VendorX", "filament", "My PETG"))

    def test_key_is_a_flat_slash_join(self):
        # The mint key is "<vendor>/<type>/<name>" with no escaping, so a "/" in
        # a component shifts the split — harmless in practice (vendor is a
        # directory name and type is one of PROFILE_SUBDIRS), but it is what the
        # C++ side does too and the two must agree byte for byte.
        self.assertEqual(afi.generate_preset_setting_id("A/B", "filament", "C"),
                         afi.generate_preset_setting_id("A", "B/filament", "C"))


class TestBase62Tail(unittest.TestCase):
    def test_hand_computed_digits(self):
        self.assertEqual(afi._base62_tail(0, 4), "0000")
        self.assertEqual(afi._base62_tail(61, 1), "z")           # last alphabet char
        self.assertEqual(afi._base62_tail(62, 2), "10")          # 1*62 + 0
        self.assertEqual(afi._base62_tail(3843, 2), "zz")        # 61*62 + 61
        self.assertEqual(afi._base62_tail(3907, 3), "111")       # 62^2 + 62 + 1

    def test_keeps_only_the_low_digits(self):
        self.assertEqual(afi._base62_tail(62, 1), "0")           # high digit dropped
        self.assertEqual(afi._base62_tail(3907, 2), "11")


# ---------------------------------------------------------------------------
# assignment
# ---------------------------------------------------------------------------

class TestAssignment(SettingTreeCase):
    def test_instantiation_is_read_exactly_as_the_validator_reads_it(self):
        # orca_extra_profile_check.py tests `instantiation == "true"` strictly.
        # Anything looser here would hand an id to a preset the validator calls
        # a base profile, and the two would fight over it on every run.
        for name, value in [("Boolean", True), ("Capitalised", "True"),
                            ("Numeric", 1), ("Absent", None)]:
            with self.subTest(instantiation=value):
                data = {"type": "filament", "name": name, "from": "system"}
                if value is not None:
                    data["instantiation"] = value
                self.t.write("VendorA", "filament", data)
        before = self.t.bytes_map()

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (0, 0), out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_a_bundle_without_an_index_is_still_assigned(self):
        # setting_id is a per-file property, and the validator walks every
        # directory. A bundle whose index has not landed yet must be fixable,
        # or the validator flags files this tool refuses to touch.
        path = os.path.join(self.t.profiles, "Noindex", "process", "Q.json")
        os.makedirs(os.path.dirname(path))
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(preset("Q", type_name="process"), f, indent=4)
        self.assertFalse(os.path.exists(self.t.index_path("Noindex")))

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 0), out)
        with open(path, encoding="utf-8") as f:
            self.assertEqual(json.load(f)["setting_id"],
                             afi.generate_preset_setting_id("Noindex", "process", "Q"))

    def test_assigns_across_every_profile_subdir(self):
        self.t.write("VendorA", "filament", preset("A PLA @P1"))
        self.t.write("VendorA", "process",
                     preset("0.20mm Standard @P1", type_name="process"))
        self.t.write("VendorA", "machine",
                     preset("P1 0.4 nozzle", type_name="machine"))
        # os.walk recursion: a preset in a nested directory is walked too.
        self.t.write("VendorA", os.path.join("filament", "nested"),
                     preset("A PETG @P1"))

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (4, 0), out)
        for subdir, type_name, name in [
                ("filament", "filament", "A PLA @P1"),
                ("process", "process", "0.20mm Standard @P1"),
                ("machine", "machine", "P1 0.4 nozzle"),
                (os.path.join("filament", "nested"), "filament", "A PETG @P1")]:
            self.assertEqual(
                self.t.read("VendorA", subdir, name)["setting_id"],
                afi.generate_preset_setting_id("VendorA", type_name, name),
                msg=name)

    def test_type_comes_from_the_subdirectory_not_the_type_field(self):
        # The subdir name is the type name (Preset::get_type_string()); a stale
        # "type" field inside the file does not enter the id.
        self.t.write("VendorA", "process", preset("Odd @P1", type_name="filament"))
        changed, errors, out = self.t.run()
        self.assertEqual((changed, errors), (1, 0), out)
        self.assertEqual(self.t.read("VendorA", "process", "Odd @P1")["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "process", "Odd @P1"))

    def test_idempotent(self):
        self.t.write("VendorA", "filament", preset("A PLA @P1"))
        self.t.write("VendorA", "process",
                     preset("0.20mm Standard @P1", type_name="process"))
        self.t.write("VendorA", "filament", preset("A PLA @base", instantiation=False,
                                                   setting_id="LEFTOVER00000000"))
        changed, errors, out = self.t.run()
        self.assertEqual((changed, errors), (3, 0), out)

        after_first = self.t.bytes_map()
        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (0, 0), out)
        self.assertEqual(self.t.bytes_map(), after_first)

    def test_stale_value_is_replaced_in_place(self):
        path = self.t.write("VendorA", "filament",
                            preset("A PLA @P1", setting_id="0000000000000000",
                                   filament_id="OFabc123"))
        before = self.t.raw("VendorA", "filament", "A PLA @P1")

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 0), out)
        want = afi.generate_preset_setting_id("VendorA", "filament", "A PLA @P1")
        self.assertEqual(self.t.read("VendorA", "filament", "A PLA @P1")["setting_id"],
                         want)
        raw = self.t.raw("VendorA", "filament", "A PLA @P1")
        self.assertEqual(raw.count(b'"setting_id"'), 1)  # replaced, not appended
        self.assertEqual(raw, before.replace(b'"0000000000000000"',
                                             b'"%s"' % want.encode()))
        self.assertTrue(os.path.isfile(path))

    def test_missing_value_is_inserted_before_filament_id(self):
        self.t.write("VendorA", "filament",
                     preset("A PLA @P1", filament_id="OFabc123"))
        changed, errors, out = self.t.run()
        self.assertEqual((changed, errors), (1, 0), out)
        text = self.t.raw("VendorA", "filament", "A PLA @P1").decode("utf-8")
        want = afi.generate_preset_setting_id("VendorA", "filament", "A PLA @P1")
        self.assertIn(f'"setting_id": "{want}"', text)
        self.assertLess(text.index('"setting_id"'), text.index('"filament_id"'))

    def test_base_profiles_are_stripped(self):
        self.t.write("VendorA", "filament",
                     preset("A PLA @base", instantiation=False,
                            setting_id="0000000000000000", filament_id="OFabc123"))
        changed, errors, out = self.t.run()
        self.assertEqual((changed, errors), (1, 0), out)
        data = self.t.read("VendorA", "filament", "A PLA @base")
        self.assertNotIn("setting_id", data)
        self.assertEqual(data["filament_id"], "OFabc123")   # nothing else touched
        self.assertEqual(data["instantiation"], "false")

    def test_base_profile_without_instantiation_key_is_stripped(self):
        # No "instantiation" key at all == not instantiated (str(None) != "true").
        data = {"type": "filament", "name": "A PLA @root", "from": "system",
                "setting_id": "0000000000000000"}
        self.t.write("VendorA", "filament", data)
        changed, errors, out = self.t.run()
        self.assertEqual((changed, errors), (1, 0), out)
        self.assertNotIn("setting_id", self.t.read("VendorA", "filament", "A PLA @root"))

    def test_misspelled_settings_id_is_dropped(self):
        self.t.write("VendorA", "filament",
                     preset("A PLA @base", instantiation=False,
                            settings_id="0000000000000000"))
        changed, errors, out = self.t.run()
        self.assertEqual((changed, errors), (1, 0), out)
        data = self.t.read("VendorA", "filament", "A PLA @base")
        self.assertNotIn("settings_id", data)
        self.assertNotIn("setting_id", data)

    def test_typo_drop_and_assignment_are_one_file_change(self):
        self.t.write("VendorA", "filament",
                     preset("A PLA @P1", settings_id="0000000000000000",
                            filament_id="OFabc123"))

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 0), out)   # ONE counted file change
        data = self.t.read("VendorA", "filament", "A PLA @P1")
        self.assertNotIn("settings_id", data)
        self.assertEqual(data["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "filament", "A PLA @P1"))

    def test_reserved_vendor_keeps_instantiated_ids_and_loses_base_ones(self):
        # BBL owns the authoritative "G*" cloud id space: its instantiated
        # presets are never rewritten, its base declarations still are stripped.
        self.t.write("BBL", "filament",
                     preset("Bambu ABS @BBL A1", setting_id="GFSB00_07"))
        self.t.write("BBL", "filament", preset("Bambu ABS @P1 no id"))
        self.t.write("BBL", "filament",
                     preset("Bambu ABS @base", instantiation=False,
                            setting_id="GFSB00_00"))
        kept = self.t.raw("BBL", "filament", "Bambu ABS @BBL A1")
        kept_idless = self.t.raw("BBL", "filament", "Bambu ABS @P1 no id")

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 0), out)   # only the base profile
        self.assertEqual(self.t.raw("BBL", "filament", "Bambu ABS @BBL A1"), kept)
        self.assertEqual(self.t.raw("BBL", "filament", "Bambu ABS @P1 no id"),
                         kept_idless)
        self.assertNotIn("setting_id",
                         self.t.read("BBL", "filament", "Bambu ABS @base"))

    def test_reserved_vendors_misspelled_key_is_corrected_not_dropped(self):
        # A reserved vendor's id is authoritative, so there is no formula to
        # fall back on. Dropping the typo and stopping there would leave the
        # preset with no setting_id at all and no way for the tool to give it
        # one - a validator error nothing can clear. Fix the key, keep the value.
        self.t.write("BBL", "filament",
                     preset("Bambu PLA @P1", settings_id="GFSA00_01"))
        # A base profile still just loses the key; it may not carry an id.
        self.t.write("BBL", "filament",
                     preset("Bambu PLA @base", instantiation=False,
                            settings_id="GFSA00_00"))

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (2, 0), out)
        fixed = self.t.read("BBL", "filament", "Bambu PLA @P1")
        self.assertNotIn("settings_id", fixed)
        self.assertEqual(fixed["setting_id"], "GFSA00_01")
        base = self.t.read("BBL", "filament", "Bambu PLA @base")
        self.assertNotIn("settings_id", base)
        self.assertNotIn("setting_id", base)
        # Idempotent: the corrected file is what the next run expects to see.
        self.assertEqual(self.t.run()[:2], (0, 0))

    def test_a_managed_vendors_misspelled_key_is_still_replaced_by_the_mint(self):
        self.t.write("VendorA", "filament",
                     preset("A PLA @P1", settings_id="whatever"))
        changed, errors, out = self.t.run()
        self.assertEqual((changed, errors), (1, 0), out)
        data = self.t.read("VendorA", "filament", "A PLA @P1")
        self.assertNotIn("settings_id", data)
        self.assertEqual(data["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "filament", "A PLA @P1"))

    def test_reserved_vendors_constant(self):
        self.assertEqual(afi.RESERVED_VENDORS, {"BBL"})
        self.assertEqual(afi.PROFILE_SUBDIRS, ("filament", "process", "machine"))


# ---------------------------------------------------------------------------
# --vendor narrowing
# ---------------------------------------------------------------------------

class TestVendorNarrowing(SettingTreeCase):
    def setUp(self):
        super().setUp()
        self.t.write("VendorA", "filament", preset("A PLA @P1"))
        self.t.write("VendorB", "filament", preset("B PLA @P1"))

    def test_restricts_writes_to_the_named_vendor(self):
        before = self.t.bytes_map()

        changed, errors, out = self.t.run(vendors=["VendorA"])

        self.assertEqual((changed, errors), (1, 0), out)
        self.assertEqual(self.t.read("VendorA", "filament", "A PLA @P1")["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "filament", "A PLA @P1"))
        self.assertEqual(self.t.raw("VendorB", "filament", "B PLA @P1"),
                         before[os.path.join("VendorB", "filament", "B PLA @P1.json")])
        # ... and the vendor left out is written by a later run, unchanged in kind.
        changed, errors, out = self.t.run()
        self.assertEqual((changed, errors), (1, 0), out)
        self.assertEqual(self.t.read("VendorB", "filament", "B PLA @P1")["setting_id"],
                         afi.generate_preset_setting_id("VendorB", "filament", "B PLA @P1"))

    def test_unknown_vendor_reports_and_writes_nothing(self):
        before = self.t.bytes_map()

        changed, errors, out = self.t.run(vendors=["Nope"])

        self.assertEqual((changed, errors), (0, 1))
        self.assertIn("Nope", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_unknown_vendor_blocks_the_known_ones_too(self):
        before = self.t.bytes_map()
        changed, errors, _out = self.t.run(vendors=["VendorA", "Nope"])
        self.assertEqual((changed, errors), (0, 1))
        self.assertEqual(self.t.bytes_map(), before)


# ---------------------------------------------------------------------------
# --dry-run
# ---------------------------------------------------------------------------

class TestDryRun(SettingTreeCase):
    def test_writes_nothing_and_previews_the_real_run(self):
        self.t.write("VendorA", "filament", preset("A PLA @P1"))
        self.t.write("VendorA", "process",
                     preset("0.20mm Standard @P1", type_name="process"))
        self.t.write("VendorA", "filament",
                     preset("A PLA @base", instantiation=False,
                            setting_id="0000000000000000"))
        before = self.t.bytes_map()

        dry_changed, dry_errors, out = self.t.run(dry_run=True)

        self.assertEqual((dry_changed, dry_errors), (3, 0), out)
        self.assertIn("would", out)
        self.assertEqual(self.t.bytes_map(), before)   # nothing written

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (dry_changed, dry_errors), out)
        self.assertNotEqual(self.t.bytes_map(), before)


# ---------------------------------------------------------------------------
# byte preservation
# ---------------------------------------------------------------------------

class TestBytePreservation(SettingTreeCase):
    CRLF_TEXT = (
        '{\r\n'
        '\t"type": "filament",\r\n'
        '\t"name": "CRLF PLA @P1",\r\n'
        '\t"from": "system",\r\n'
        '\t"filament_id": "OFabc123",\r\n'
        '\t"instantiation": "true",\r\n'
        '\t"filament_type": [\r\n'
        '\t\t"PLA"\r\n'
        '\t]\r\n'
        '}\r\n'
    )

    def test_crlf_and_tab_indentation_survive(self):
        self.t.write_raw("VendorA", "filament", "CRLF PLA @P1",
                         self.CRLF_TEXT.encode("utf-8"))

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 0), out)
        raw = self.t.raw("VendorA", "filament", "CRLF PLA @P1")
        self.assertEqual(raw.count(b"\n"), raw.count(b"\r\n"))   # still CRLF-only
        want = afi.generate_preset_setting_id("VendorA", "filament", "CRLF PLA @P1")
        inserted = ('\t"setting_id": "%s",\r\n' % want).encode("utf-8")
        # Every original byte survives: dropping the inserted line restores the file.
        self.assertEqual(raw.replace(inserted, b"", 1),
                         self.CRLF_TEXT.encode("utf-8"))

    def test_bom_survives(self):
        raw_in = b"\xef\xbb\xbf" + json.dumps(
            preset("BOM PLA @P1", filament_id="OFabc123"),
            indent=4, ensure_ascii=False).encode("utf-8") + b"\n"
        self.t.write_raw("VendorA", "filament", "BOM PLA @P1", raw_in)

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 0), out)
        raw = self.t.raw("VendorA", "filament", "BOM PLA @P1")
        self.assertTrue(raw.startswith(b"\xef\xbb\xbf"))
        self.assertEqual(raw.count(b"\xef\xbb\xbf"), 1)
        want = afi.generate_preset_setting_id("VendorA", "filament", "BOM PLA @P1")
        self.assertEqual(self.t.read("VendorA", "filament", "BOM PLA @P1")["setting_id"],
                         want)
        self.assertEqual(
            raw.replace(('    "setting_id": "%s",\n' % want).encode("utf-8"), b"", 1),
            raw_in)

    def test_non_ascii_name_round_trips(self):
        name = "拓竹 PLA @P1 0.4 nozzle"
        raw_in = (json.dumps(preset(name, filament_id="OFabc123"), indent=4,
                             ensure_ascii=False).encode("utf-8") + b"\n")
        self.t.write_raw("VendorA", "filament", name, raw_in)

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 0), out)
        raw = self.t.raw("VendorA", "filament", name)
        self.assertIn(name.encode("utf-8"), raw)         # not escaped to \uXXXX
        data = self.t.read("VendorA", "filament", name)
        self.assertEqual(data["name"], name)
        self.assertEqual(data["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "filament", name))

    def test_surrounding_formatting_is_untouched_on_a_strip(self):
        text = ('{\r\n'
                '\t"type": "filament",\r\n'
                '\t"name": "Odd @base",\r\n'
                '\t"setting_id": "0000000000000000",\r\n'
                '\t"instantiation": "false",\r\n'
                '\t"compatible_printers": []\r\n'
                '}\r\n')
        self.t.write_raw("VendorA", "filament", "Odd @base", text.encode("utf-8"))

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 0), out)
        self.assertEqual(
            self.t.raw("VendorA", "filament", "Odd @base").decode("utf-8"),
            text.replace('\t"setting_id": "0000000000000000",\r\n', "", 1))


# ---------------------------------------------------------------------------
# byte-preserving key edits
# ---------------------------------------------------------------------------

class TestInsertAnchor(unittest.TestCase):
    WITH_BOTH = ('{\n    "name": "K @P1",\n    "filament_id": "OFabc123",\n'
                 '    "instantiation": "true"\n}\n')
    WITH_INSTANTIATION = ('{\n    "name": "K @P1",\n'
                          '    "instantiation": "true"\n}\n')

    def test_inserts_before_filament_id(self):
        text, n = afi.insert_setting_id(self.WITH_BOTH, "0123456789abcdef")
        self.assertEqual(n, 1)
        json.loads(text)
        self.assertEqual(text, self.WITH_BOTH.replace(
            '    "filament_id"',
            '    "setting_id": "0123456789abcdef",\n    "filament_id"', 1))

    def test_falls_back_to_instantiation(self):
        text, n = afi.insert_setting_id(self.WITH_INSTANTIATION, "0123456789abcdef")
        self.assertEqual(n, 1)
        json.loads(text)
        self.assertIn('"setting_id": "0123456789abcdef",\n    "instantiation"', text)

    def test_falls_back_to_name(self):
        # Last-resort anchor: every preset has a name, so the insert does not
        # depend on filament_id having been written first — a dry run, which
        # writes none, must reach the same verdict as the real run.
        text, n = afi.insert_setting_id('{\n    "name": "K",\n    "x": 1\n}\n',
                                        "0123456789abcdef")
        self.assertEqual(n, 1)
        json.loads(text)
        self.assertIn('"name": "K",\n    "setting_id": "0123456789abcdef",', text)

    def test_no_anchor_returns_zero(self):
        src = '{\n    "type": "filament"\n}\n'
        text, n = afi.insert_setting_id(src, "0123456789abcdef")
        self.assertEqual(n, 0)
        self.assertEqual(text, src)


class TestKeyLineHelpers(unittest.TestCase):
    def test_delete_trailing_comma_form(self):
        text = ('{\n    "name": "K",\n    "setting_id": "OLD0000000000000",\n'
                '    "instantiation": "false"\n}\n')
        out, n = afi.delete_key_line(text, "setting_id")
        self.assertEqual(n, 1)
        self.assertEqual(json.loads(out), {"name": "K", "instantiation": "false"})
        self.assertEqual(out, text.replace(
            '    "setting_id": "OLD0000000000000",\n', "", 1))

    def test_delete_last_property_form_consumes_the_preceding_comma(self):
        text = ('{\n    "name": "K",\n    "instantiation": "false",\n'
                '    "setting_id": "OLD0000000000000"\n}\n')
        out, n = afi.delete_key_line(text, "setting_id")
        self.assertEqual(n, 1)
        self.assertEqual(json.loads(out), {"name": "K", "instantiation": "false"})
        self.assertEqual(out, '{\n    "name": "K",\n    "instantiation": "false"\n}\n')

    def test_delete_requires_the_exact_old_value(self):
        text = ('{\n    "name": "K",\n    "setting_id": "OLD0000000000000",\n'
                '    "instantiation": "false"\n}\n')
        out, n = afi.delete_key_line(text, "setting_id", old_value="OTHER")
        self.assertEqual((out, n), (text, 0))
        _out, n = afi.delete_key_line(text, "setting_id",
                                      old_value="OLD0000000000000")
        self.assertEqual(n, 1)

    def test_delete_missing_key_returns_zero(self):
        text = '{\n    "name": "K"\n}\n'
        self.assertEqual(afi.delete_key_line(text, "setting_id"), (text, 0))

    def test_delete_does_not_confuse_the_two_spellings(self):
        text = ('{\n    "name": "K",\n    "settings_id": "TYPO000000000000",\n'
                '    "setting_id": "REAL000000000000",\n'
                '    "instantiation": "true"\n}\n')
        out, n = afi.delete_key_line(text, "settings_id")
        self.assertEqual(n, 1)
        self.assertEqual(json.loads(out)["setting_id"], "REAL000000000000")

    def test_replace_refuses_a_stale_old_value(self):
        text = '{\n    "setting_id": "OLD0000000000000"\n}\n'
        out, n = afi.replace_key_value(text, "setting_id", "NEW0000000000000",
                                       old_value="NOTTHIS000000000")
        self.assertEqual((out, n), (text, 0))
        out, n = afi.replace_key_value(text, "setting_id", "NEW0000000000000",
                                       old_value="OLD0000000000000")
        self.assertEqual(n, 1)
        self.assertEqual(json.loads(out)["setting_id"], "NEW0000000000000")

    def test_insert_key_line_prefers_before_over_after(self):
        text = ('{\n    "name": "K",\n    "filament_id": "OFabc123",\n'
                '    "instantiation": "true"\n}\n')
        out, n = afi.insert_key_line(text, "setting_id", "V",
                                     before=("filament_id", "instantiation"),
                                     after=("name",))
        self.assertEqual(n, 1)
        self.assertLess(out.index('"setting_id"'), out.index('"filament_id"'))
        self.assertGreater(out.index('"setting_id"'), out.index('"name"'))
        # The `before` tuple's own order decides, not the order in the file.
        out, _n = afi.insert_key_line(text, "setting_id", "V",
                                      before=("instantiation", "filament_id"))
        self.assertGreater(out.index('"setting_id"'), out.index('"filament_id"'))

    def test_insert_key_line_falls_back_to_after(self):
        text = '{\n    "name": "K",\n    "from": "system"\n}\n'
        out, n = afi.insert_key_line(text, "setting_id", "V",
                                     before=("filament_id",), after=("name",))
        self.assertEqual(n, 1)
        self.assertEqual(out, '{\n    "name": "K",\n    "setting_id": "V",\n'
                              '    "from": "system"\n}\n')

    def test_insert_key_line_without_any_anchor(self):
        text = '{\n    "from": "system"\n}\n'
        self.assertEqual(
            afi.insert_key_line(text, "setting_id", "V", before=("filament_id",),
                                after=("name",)),
            (text, 0))


# ---------------------------------------------------------------------------
# error paths: reported and counted, never raised
# ---------------------------------------------------------------------------

class TestErrorPaths(SettingTreeCase):
    def test_unparsable_profile_is_reported_and_the_run_continues(self):
        broken = b'{\n    "name": "Broken @P1",\n    oops\n}\n'
        self.t.write_raw("VendorA", "filament", "Broken @P1", broken)
        self.t.write("VendorA", "filament", preset("Good @P1"))

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 1), out)
        self.assertIn("Broken @P1", out)
        self.assertEqual(self.t.raw("VendorA", "filament", "Broken @P1"), broken)
        self.assertEqual(self.t.read("VendorA", "filament", "Good @P1")["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "filament", "Good @P1"))

    def test_non_object_top_level_is_reported(self):
        raw = b'[\n    {"name": "K"}\n]\n'
        self.t.write_raw("VendorA", "filament", "List @P1", raw)
        self.t.write("VendorA", "filament", preset("Good @P1"))

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 1), out)
        self.assertIn("List @P1", out)
        self.assertEqual(self.t.raw("VendorA", "filament", "List @P1"), raw)

    def test_nameless_instantiated_preset_is_reported(self):
        nameless = {"type": "filament", "from": "system", "instantiation": "true"}
        self.t.write("VendorA", "filament", nameless, name="Nameless")
        self.t.write("VendorA", "filament", preset("Good @P1"))
        before = self.t.raw("VendorA", "filament", "Nameless")

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 1), out)
        self.assertIn("Nameless", out)
        self.assertEqual(self.t.raw("VendorA", "filament", "Nameless"), before)
        self.assertEqual(self.t.read("VendorA", "filament", "Good @P1")["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "filament", "Good @P1"))

    def test_a_nameless_preset_still_gets_its_misspelled_key_dropped(self):
        # The nameless-preset error must not abandon an edit already queued for
        # the same file: leaving "settings_id" behind would keep the validator
        # red with no way for this tool to clear it.
        nameless = {"type": "filament", "from": "system", "settings_id": "JUNK123",
                    "instantiation": "true"}
        self.t.write("VendorA", "filament", nameless, name="Nameless")

        changed, errors, out = self.t.run()

        self.assertEqual((changed, errors), (1, 1), out)
        self.assertNotIn("settings_id", self.t.read("VendorA", "filament", "Nameless"))
        self.assertNotIn("setting_id", self.t.read("VendorA", "filament", "Nameless"))
        self.assertIn('misspelled "settings_id" dropped : 1', out)

    def test_an_unanchorable_file_is_reported_not_raised(self):
        # One oddly formatted profile must not abort the pass over all the
        # others, and it must fail the same way in a dry run as in a real one.
        raw = b'{ "type":"filament", "name":"Flat @P1", "instantiation":"true" }\n'
        self.t.write_raw("VendorA", "filament", "Flat @P1", raw)
        self.t.write("VendorA", "filament", preset("Good @P1"))

        dry_changed, dry_errors, dry_out = self.t.run(dry_run=True)
        changed, errors, out = self.t.run()

        self.assertEqual((dry_changed, dry_errors), (changed, errors), dry_out)
        self.assertEqual((changed, errors), (1, 1), out)
        self.assertIn("Flat @P1", out)
        self.assertEqual(self.t.raw("VendorA", "filament", "Flat @P1"), raw)
        self.assertEqual(self.t.read("VendorA", "filament", "Good @P1")["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "filament", "Good @P1"))

    def test_nameless_base_preset_is_fine(self):
        # Only instantiated presets need an identity; a nameless base profile is
        # simply left alone.
        nameless = {"type": "filament", "from": "system", "instantiation": "false"}
        self.t.write("VendorA", "filament", nameless, name="Nameless base")
        before = self.t.bytes_map()
        changed, errors, out = self.t.run()
        self.assertEqual((changed, errors), (0, 0), out)
        self.assertEqual(self.t.bytes_map(), before)


# ---------------------------------------------------------------------------
# the real tree
# ---------------------------------------------------------------------------

@unittest.skipUnless(os.path.isdir(REAL_PROFILES), "resources/profiles not present")
class TestRealTree(unittest.TestCase):
    def test_shipped_tree_needs_no_setting_id_change(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            changed, errors = afi.generate_setting_ids(REAL_PROFILES, dry_run=True)
        self.assertEqual((changed, errors), (0, 0), buf.getvalue())


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

class TestCli(SettingTreeCase):
    def main(self, argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = afi.main(argv)
        return rc, buf.getvalue()

    def test_setting_id_only_run_touches_no_filament_id(self):
        # "OFZZZZZZ" is not the mint of the preset's own triple, so the filament
        # half of --generate has real work waiting on this tree (proven at the
        # end): leaving the id alone is the narrowing's doing, not an idle tree.
        self.t.write("VendorA", "filament",
                     preset("A PLA @P1", filament_id="OFZZZZZZ",
                            filament_vendor=["AV"], filament_type=["PLA"]))
        self.t.write("VendorA", "machine",
                     preset("P1 0.4 nozzle", type_name="machine"))

        rc, out = self.main(["--generate", "--setting-id",
                             "--profiles", self.t.profiles])

        self.assertEqual(rc, 0, out)
        filament = self.t.read("VendorA", "filament", "A PLA @P1")
        self.assertEqual(filament["filament_id"], "OFZZZZZZ")
        self.assertEqual(filament["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "filament", "A PLA @P1"))
        machine = self.t.read("VendorA", "machine", "P1 0.4 nozzle")
        self.assertNotIn("filament_id", machine)
        self.assertEqual(machine["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "machine", "P1 0.4 nozzle"))
        # ... and the skipped half does re-mint that id when it is allowed to run.
        changed, errors, out = self.t.run_filament_ids()
        self.assertEqual((changed, errors), (1, 0), out)
        self.assertEqual(
            self.t.read("VendorA", "filament", "A PLA @P1")["filament_id"],
            afi.generate_filament_id("AV", "PLA", "A PLA"))

    def test_dry_run_setting_id_writes_nothing(self):
        self.t.write("VendorA", "filament", preset("A PLA @P1"))
        before = self.t.bytes_map()
        rc, out = self.main(["--generate", "--setting-id", "--dry-run",
                             "--profiles", self.t.profiles])
        self.assertEqual(rc, 0, out)
        self.assertIn("1 file(s) would change", out)   # there WAS one to write
        self.assertEqual(self.t.bytes_map(), before)
        # the real run then writes exactly it
        rc, out = self.main(["--generate", "--setting-id",
                             "--profiles", self.t.profiles])
        self.assertEqual(rc, 0, out)
        self.assertIn("1 file(s) changed", out)
        self.assertEqual(self.t.read("VendorA", "filament", "A PLA @P1")["setting_id"],
                         afi.generate_preset_setting_id("VendorA", "filament",
                                                        "A PLA @P1"))

    def test_setting_id_without_generate_is_a_usage_error(self):
        with contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit) as cm:
            afi.main(["--setting-id", "--profiles", self.t.profiles])
        self.assertEqual(cm.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
