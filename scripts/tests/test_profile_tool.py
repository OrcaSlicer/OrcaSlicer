#!/usr/bin/env python3
"""Tests for the tree-maintenance half of scripts/orca_profile_tool.py: the
normalize, trim, update-index and check commands, and the subcommand dispatch that
reaches them (stdlib unittest, no external deps).

The id halves are covered by test_filament_id.py and test_setting_id.py.

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

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import orca_profile_tool as apt  # noqa: E402

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REAL_PROFILES = os.path.join(REPO_ROOT, "resources", "profiles")


class Tree:
    """A throwaway resources/profiles-shaped directory built one file at a time.

    Nothing is written implicitly: index entries are added by index(), so a test
    can produce exactly the mismatch it is about (a file no list references, a
    list naming a file that is not there, a preset whose name disagrees with the
    index).
    """

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="profile_tool_test_")
        self.profiles = os.path.join(self.dir, "profiles")
        os.makedirs(self.profiles)

    def cleanup(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def index_path(self, vendor):
        return os.path.join(self.profiles, vendor + ".json")

    def add_vendor(self, vendor):
        for sub in apt.PROFILE_SUBDIRS:
            os.makedirs(os.path.join(self.profiles, vendor, sub), exist_ok=True)
        if not os.path.exists(self.index_path(vendor)):
            self.write_index(vendor, {"name": vendor, "version": "01.00.00.00"})
        return self

    def write_index(self, vendor, index):
        with open(self.index_path(vendor), "w", encoding="utf-8", newline="\n") as f:
            json.dump(index, f, indent=4, ensure_ascii=False)
            f.write("\n")

    def read_index(self, vendor):
        with open(self.index_path(vendor), encoding="utf-8-sig") as f:
            return json.load(f)

    def index(self, vendor, section, name, sub_path):
        index = self.read_index(vendor)
        index.setdefault(section + "_list", []).append(
            {"name": name, "sub_path": sub_path})
        self.write_index(vendor, index)

    def path(self, vendor, rel):
        return os.path.join(self.profiles, vendor, rel.replace("/", os.sep))

    def write(self, vendor, rel, data):
        """Write a preset at <vendor>/<rel>; returns its path."""
        self.add_vendor(vendor)
        path = self.path(vendor, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(data, f, indent=4, ensure_ascii=False)
            f.write("\n")
        return path

    def write_raw(self, vendor, rel, raw):
        self.add_vendor(vendor)
        path = self.path(vendor, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(raw)
        return path

    def read(self, vendor, rel):
        with open(self.path(vendor, rel), encoding="utf-8-sig") as f:
            return json.load(f)

    def raw(self, vendor, rel):
        with open(self.path(vendor, rel), "rb") as f:
            return f.read()

    def bytes_map(self):
        """Every file in the tree -> its bytes, for "nothing was written" asserts."""
        out = {}
        for root, dirs, files in os.walk(self.profiles):
            dirs.sort()
            for name in sorted(files):
                path = os.path.join(root, name)
                with open(path, "rb") as f:
                    out[os.path.relpath(path, self.profiles)] = f.read()
        return out


class TreeCase(unittest.TestCase):
    def setUp(self):
        self.t = Tree()
        self.addCleanup(self.t.cleanup)

    def run_command(self, *argv):
        """main() against this tree, capturing stdout."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = apt.main([*argv, "--profiles", self.t.profiles])
        return rc, buf.getvalue()


# ---------------------------------------------------------------------------
# normalize
# ---------------------------------------------------------------------------

class TestObsoleteKeys(unittest.TestCase):
    def test_obsolete_keys_match_the_loader_ignore_set(self):
        path = os.path.join(REPO_ROOT, "src", "libslic3r", "PrintConfig.cpp")
        with open(path, encoding="utf-8") as f:
            source = f.read()
        match = re.search(
            r"void PrintConfigDef::handle_legacy\(.*?"
            r"static\s+std::set<std::string>\s+ignore\s*=\s*\{(.*?)\};",
            source, re.DOTALL)
        self.assertIsNotNone(match, "Could not locate the loader's obsolete-key set")
        keys = re.sub(r"//[^\n]*|/\*.*?\*/", "", match.group(1), flags=re.DOTALL)
        self.assertEqual(apt.OBSOLETE_KEYS, set(re.findall(r'"([^"\n]+)"', keys)))


class TestNormalize(TreeCase):
    def test_a_missing_type_is_filled_in_from_the_directory(self):
        self.t.write("V", "filament/A.json", {"name": "A"})
        self.t.write("V", "process/B.json", {"name": "B"})
        rc, out = self.run_command("normalize")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.t.read("V", "filament/A.json")["type"], "filament")
        self.assertEqual(self.t.read("V", "process/B.json")["type"], "process")

    def test_the_machine_folder_splits_on_the_preset_name(self):
        # Orca keeps machine models in machine/ next to the nozzle variants that
        # are machines; only the name tells them apart.
        self.t.write("V", "machine/M.json", {"name": "V Printer"})
        self.t.write("V", "machine/N.json", {"name": "V Printer 0.4 nozzle"})
        rc, out = self.run_command("normalize")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.t.read("V", "machine/M.json")["type"], "machine_model")
        self.assertEqual(self.t.read("V", "machine/N.json")["type"], "machine")

    def test_dropped_keys_go_and_filament_vectors_are_arrayified(self):
        self.t.write("V", "filament/A.json", {
            "type": "filament", "name": "A", "version": "1.2.3",
            "is_custom_defined": "1", "filament_type": "PLA",
            "filament_vendor": "AV", "travel_speed": 200,
            "filament_load_time": ["15"], "filament_unload_time": "0"})
        rc, out = self.run_command("normalize")
        self.assertEqual(rc, 0, out)
        data = self.t.read("V", "filament/A.json")
        self.assertNotIn("version", data)
        self.assertNotIn("is_custom_defined", data)
        self.assertNotIn("travel_speed", data)   # a process setting, not a filament one
        self.assertNotIn("filament_load_time", data)
        self.assertNotIn("filament_unload_time", data)
        self.assertEqual(data["filament_type"], ["PLA"])
        self.assertEqual(data["filament_vendor"], ["AV"])

    def test_obsolete_keys_are_removed_from_every_profile_type(self):
        for sub in ("filament", "process", "machine"):
            with self.subTest(profile_type=sub):
                expected = {"type": sub, "name": "A"}
                self.t.write("V", f"{sub}/A.json", {
                    **expected, "silent_mode": "", "adaptive_layer_height": "0",
                    "anisotropic_surfaces": "1", "filament_load_time": ["0"],
                    "filament_unload_time": "0"})
                rc, out = self.run_command("normalize")
                self.assertEqual(rc, 0, out)
                self.assertEqual(self.t.read("V", f"{sub}/A.json"), expected)
        before = self.t.bytes_map()
        rc, out = self.run_command("normalize")
        self.assertEqual(rc, 0, out)
        self.assertIn("0 profile(s) normalized", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_the_larger_extruder_clearance_wins(self):
        # Keeping the smaller one would licence a toolhead collision.
        self.t.write("V", "machine/M.json", {
            "type": "machine", "name": "M 0.4 nozzle",
            "extruder_clearance_radius": "45", "extruder_clearance_max_radius": "68"})
        self.t.write("V", "machine/N.json", {
            "type": "machine", "name": "N 0.4 nozzle",
            "extruder_clearance_radius": "68", "extruder_clearance_max_radius": "45"})
        rc, out = self.run_command("normalize")
        self.assertEqual(rc, 0, out)
        kept = self.t.read("V", "machine/M.json")
        self.assertNotIn("extruder_clearance_radius", kept)
        self.assertEqual(kept["extruder_clearance_max_radius"], "68")
        kept = self.t.read("V", "machine/N.json")
        self.assertNotIn("extruder_clearance_max_radius", kept)
        self.assertEqual(kept["extruder_clearance_radius"], "68")

    def test_a_rewritten_file_leads_with_its_identifying_keys(self):
        self.t.write("V", "filament/A.json", {
            "filament_cost": [20], "name": "A", "instantiation": "true",
            "inherits": "base", "version": "1"})
        rc, out = self.run_command("normalize")
        self.assertEqual(rc, 0, out)
        keys = list(self.t.read("V", "filament/A.json"))
        self.assertEqual(keys[:4], ["type", "name", "inherits", "instantiation"])

    def test_a_conforming_tree_is_left_byte_identical(self):
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        # These used to be misclassified as obsolete: one is active, the other
        # is a legacy alias that still supplies the toolhead clearance on load.
        self.t.write("V", "machine/M.json", {
            "type": "machine", "name": "M", "extruder_type": ["Direct Drive"],
            "extruder_clearance_max_radius": "68", "machine_load_filament_time": "15",
            "machine_unload_filament_time": "10"})
        self.t.write("V", "process/P.json", {
            "type": "process", "name": "P", "travel_speed": "200",
            "top_surface_fill_order": "outward"})
        before = self.t.bytes_map()
        rc, out = self.run_command("normalize")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_force_rewrites_even_a_conforming_file(self):
        self.t.write_raw("V", "filament/A.json",
                         b'{"name":"A","type":"filament"}')
        rc, out = self.run_command("normalize", "--force")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.t.raw("V", "filament/A.json"),
                         b'{\n\t"type": "filament",\n\t"name": "A"\n}\n')

    def test_dry_run_writes_nothing(self):
        self.t.write("V", "filament/A.json", {"name": "A", "bed_temperature": ["60"]})
        before = self.t.bytes_map()
        rc, out = self.run_command("normalize", "--dry-run")
        self.assertEqual(rc, 0, out)
        self.assertIn("would be", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_profile_type_confines_the_run(self):
        self.t.write("V", "filament/A.json", {"name": "A"})
        self.t.write("V", "process/B.json", {"name": "B"})
        rc, out = self.run_command("normalize", "--profile-type", "filament")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.t.read("V", "filament/A.json")["type"], "filament")
        self.assertNotIn("type", self.t.read("V", "process/B.json"))

    def test_an_unreadable_profile_is_reported_not_swallowed(self):
        self.t.write_raw("V", "filament/A.json", b"{ not json")
        rc, out = self.run_command("normalize")
        self.assertEqual(rc, 1, out)
        self.assertIn("ERROR", out)
        self.assertEqual(self.t.raw("V", "filament/A.json"), b"{ not json")

    def test_a_directory_without_an_index_is_not_a_bundle(self):
        # resources/profiles also holds non-bundle entries (blacklist.json, the
        # untracked user/ directory); only a directory WITH an index is a vendor.
        stray = os.path.join(self.t.profiles, "user", "filament")
        os.makedirs(stray)
        with open(os.path.join(stray, "A.json"), "wb") as f:
            f.write(b'{"name": "A"}')
        self.t.write("V", "filament/A.json", {"name": "A"})
        rc, out = self.run_command("normalize")
        self.assertEqual(rc, 0, out)
        self.assertFalse(os.path.exists(os.path.join(self.t.profiles, "user.json")))
        with open(os.path.join(stray, "A.json"), "rb") as f:
            self.assertEqual(f.read(), b'{"name": "A"}')


# ---------------------------------------------------------------------------
# trim
# ---------------------------------------------------------------------------

class TestTrim(TreeCase):
    def bundle(self):
        self.t.write("V", "filament/Listed.json",
                     {"type": "filament", "name": "Listed"})
        self.t.index("V", "filament", "Listed", "filament/Listed.json")
        return self.t

    def test_an_unindexed_preset_is_removed(self):
        self.bundle().write("V", "filament/Orphan.json",
                            {"type": "filament", "name": "Orphan"})
        rc, out = self.run_command("trim")
        self.assertEqual(rc, 0, out)
        self.assertTrue(os.path.exists(self.t.path("V", "filament/Listed.json")))
        self.assertFalse(os.path.exists(self.t.path("V", "filament/Orphan.json")))

    def test_a_dotted_sub_path_still_names_its_file(self):
        # Index entries are hand-written; "filament/./X.json" is the same file.
        self.bundle()
        self.t.write("V", "filament/Dotted.json",
                     {"type": "filament", "name": "Dotted"})
        self.t.index("V", "filament", "Dotted", "filament/./Dotted.json")
        rc, out = self.run_command("trim")
        self.assertEqual(rc, 0, out)
        self.assertTrue(os.path.exists(self.t.path("V", "filament/Dotted.json")))

    def test_an_unparsable_file_is_kept_and_reported(self):
        self.bundle().write_raw("V", "filament/Broken.json", b"{ not json")
        rc, out = self.run_command("trim")
        self.assertEqual(rc, 0, out)
        self.assertIn("WARNING", out)
        self.assertTrue(os.path.exists(self.t.path("V", "filament/Broken.json")))

    def test_a_data_file_is_not_a_preset(self):
        self.bundle().write("V", "filament/filaments_color_codes.json",
                            {"data": [], "total": 0})
        rc, out = self.run_command("trim")
        self.assertEqual(rc, 0, out)
        self.assertTrue(os.path.exists(
            self.t.path("V", "filament/filaments_color_codes.json")))

    def test_an_inherited_base_is_kept_and_reported(self):
        # Neither file loads -- the loader only reads indexed sub_paths -- but
        # deleting the parent destroys the only record of what the indexed child
        # was written against, so that is a maintainer's call, not trim's.
        self.bundle()
        self.t.write("V", "machine/base.json",
                     {"type": "machine", "name": "V base"})
        self.t.write("V", "machine/mid.json",
                     {"type": "machine", "name": "V mid", "inherits": "V base"})
        self.t.write("V", "machine/M.json",
                     {"type": "machine", "name": "M 0.4 nozzle", "inherits": "V mid"})
        self.t.index("V", "machine", "M 0.4 nozzle", "machine/M.json")
        rc, out = self.run_command("trim")
        self.assertEqual(rc, 0, out)
        # ... and the chain is followed: mid rescues base in a second pass.
        self.assertTrue(os.path.exists(self.t.path("V", "machine/mid.json")))
        self.assertTrue(os.path.exists(self.t.path("V", "machine/base.json")))
        self.assertIn("inherited from", out)

    def test_a_stale_copy_of_an_indexed_profile_is_removed(self):
        # "inherits" resolves by preset name, so the indexed base is the parent the
        # child actually gets; the unindexed twin is a leftover the loader never
        # reaches, and being named in an inherits does not earn it a reprieve.
        self.bundle()
        self.t.write("V", "machine/HSN/base.json",
                     {"type": "machine", "name": "V base"})
        self.t.index("V", "machine", "V base", "machine/HSN/base.json")
        self.t.write("V", "machine/base.json",
                     {"type": "machine", "name": "V base"})
        self.t.write("V", "machine/M.json",
                     {"type": "machine", "name": "M 0.4 nozzle", "inherits": "V base"})
        self.t.index("V", "machine", "M 0.4 nozzle", "machine/M.json")
        rc, out = self.run_command("trim")
        self.assertEqual(rc, 0, out)
        self.assertTrue(os.path.exists(self.t.path("V", "machine/HSN/base.json")))
        self.assertFalse(os.path.exists(self.t.path("V", "machine/base.json")))
        self.assertNotIn("WARNING", out)
        self.assertIn('machine/HSN/base.json is the profile named "V base"', out)

    def test_dry_run_deletes_nothing(self):
        self.bundle().write("V", "filament/Orphan.json",
                            {"type": "filament", "name": "Orphan"})
        before = self.t.bytes_map()
        rc, out = self.run_command("trim", "--dry-run")
        self.assertEqual(rc, 0, out)
        self.assertIn("would be removed", out)
        self.assertEqual(self.t.bytes_map(), before)


# ---------------------------------------------------------------------------
# update-index
# ---------------------------------------------------------------------------

class TestUpdateIndex(TreeCase):
    def test_every_profile_on_disk_lands_in_its_own_section(self):
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        self.t.write("V", "process/B.json", {"type": "process", "name": "B"})
        self.t.write("V", "machine/M.json", {"type": "machine", "name": "M"})
        self.t.write("V", "machine/MM.json", {"type": "machine_model", "name": "MM"})
        rc, out = self.run_command("update-index")
        self.assertEqual(rc, 0, out)
        index = self.t.read_index("V")
        self.assertEqual(index["filament_list"],
                         [{"name": "A", "sub_path": "filament/A.json"}])
        self.assertEqual(index["process_list"],
                         [{"name": "B", "sub_path": "process/B.json"}])
        self.assertEqual(index["machine_list"],
                         [{"name": "M", "sub_path": "machine/M.json"}])
        self.assertEqual(index["machine_model_list"],
                         [{"name": "MM", "sub_path": "machine/MM.json"}])

    def test_parents_are_listed_before_their_children(self):
        # The loader resolves inherits in one pass over the list.
        for name, parent in (("C", "B"), ("A", None), ("B", "A")):
            data = {"type": "filament", "name": name}
            if parent:
                data["inherits"] = parent
            self.t.write("V", f"filament/{name}.json", data)
        rc, out = self.run_command("update-index")
        self.assertEqual(rc, 0, out)
        self.assertEqual([e["name"] for e in self.t.read_index("V")["filament_list"]],
                         ["A", "B", "C"])
        # inherits is ordering input only; it never lands in the index.
        for entry in self.t.read_index("V")["filament_list"]:
            self.assertEqual(sorted(entry), ["name", "sub_path"])

    def test_a_profile_with_no_usable_type_is_reported_not_dropped(self):
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        self.t.write("V", "filament/B.json", {"name": "B"})
        rc, out = self.run_command("update-index")
        self.assertEqual(rc, 1, out)
        self.assertIn("cannot be indexed", out)
        self.assertIn("filament/B.json", out)

    def test_two_profiles_claiming_one_name_leave_the_index_alone(self):
        # The bundle holds one profile per name, so a rebuild would pick a winner by
        # directory order and drop the other without a word.
        self.t.write("V", "machine/base.json", {"type": "machine", "name": "base"})
        self.t.write("V", "machine/HSN/base.json", {"type": "machine", "name": "base"})
        self.t.index("V", "machine", "base", "machine/HSN/base.json")
        before = self.t.bytes_map()
        rc, out = self.run_command("update-index")
        self.assertEqual(rc, 1, out)
        self.assertIn('2 profiles are named "base"', out)
        self.assertIn("machine/base.json", out)
        self.assertIn("machine/HSN/base.json", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_profile_type_rebuilds_only_that_section(self):
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        self.t.write("V", "process/B.json", {"type": "process", "name": "B"})
        rc, out = self.run_command("update-index", "--profile-type", "filament")
        self.assertEqual(rc, 0, out)
        index = self.t.read_index("V")
        self.assertEqual([e["name"] for e in index["filament_list"]], ["A"])
        self.assertNotIn("process_list", index)

    def test_an_up_to_date_index_is_left_byte_identical(self):
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        self.run_command("update-index")
        before = self.t.bytes_map()
        rc, out = self.run_command("update-index")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_dry_run_writes_nothing(self):
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        before = self.t.bytes_map()
        rc, out = self.run_command("update-index", "--dry-run")
        self.assertEqual(rc, 0, out)
        self.assertIn("would be rebuilt", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_a_json_file_with_no_bundle_is_never_touched(self):
        # resources/profiles/blacklist.json is a .json with no directory beside
        # it. Enumerating vendors by stem once wrote four empty *_list keys into it.
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        stray = os.path.join(self.t.profiles, "blacklist.json")
        with open(stray, "wb") as f:
            f.write(b'{"filament": ["GFSA03"]}')
        rc, out = self.run_command("update-index")
        self.assertEqual(rc, 0, out)
        with open(stray, "rb") as f:
            self.assertEqual(f.read(), b'{"filament": ["GFSA03"]}')


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------

class TestCheck(TreeCase):
    def bundle(self):
        """A bundle that passes every per-vendor check."""
        self.t.write("V", "filament/A.json", {
            "type": "filament", "name": "A", "instantiation": "true",
            "filament_id": "OFaaaaaa", "filament_type": ["PLA"],
            "filament_vendor": ["AV"], "compatible_printers": ["M 0.4 nozzle"],
            "setting_id": apt.generate_preset_setting_id("V", "filament", "A")})
        self.t.index("V", "filament", "A", "filament/A.json")
        return self.t

    def per_vendor_errors(self, *argv):
        """Run the per-vendor checks alone, which is what --vendor narrows."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = apt.check_filament_compatible_printers(self.t.profiles, "V")
            name_errors, _warn = apt.check_name_consistency(self.t.profiles, "V")
            errors += name_errors
            errors += apt.check_vector_type_keys(self.t.profiles, "V")
            errors += apt.check_filament_id_length(self.t.profiles, "V")
            conflict, _warn = apt.check_conflict_keys(self.t.profiles, "V")
            errors += conflict
            materials, _warn = apt.check_machine_default_materials(self.t.profiles, "V")
            errors += materials
        return errors, buf.getvalue()

    def test_a_clean_bundle_reports_nothing(self):
        self.bundle()
        errors, out = self.per_vendor_errors()
        self.assertEqual(errors, 0, out)

    def test_an_instantiated_filament_needs_compatible_printers(self):
        self.bundle().write("V", "filament/B.json", {
            "type": "filament", "name": "B", "instantiation": "true"})
        errors, out = self.per_vendor_errors()
        self.assertGreater(errors, 0)
        self.assertIn("'compatible_printers' missing", out)

    def test_a_library_filament_may_leave_compatible_printers_empty(self):
        # The shared library is exempt from that rule and nothing else.
        self.t.write(apt.OFL, "filament/A.json",
                     {"type": "filament", "name": "A", "instantiation": "true"})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = apt.check_filament_compatible_printers(self.t.profiles, apt.OFL)
        self.assertEqual(errors, 0, buf.getvalue())

    def test_the_library_is_checked_like_any_other_bundle(self):
        # A file the library's own index does not reference must fail plain
        # `check`, now that the per-vendor pass no longer skips it.
        self.t.write(apt.OFL, "filament/Stray.json",
                     {"type": "filament", "name": "Stray"})
        snapshot = os.path.join(self.t.dir, "snapshot.json")
        self.run_command("update-snapshot", "--snapshot", snapshot)
        rc, out = self.run_command("check", "--snapshot", snapshot)
        self.assertEqual(rc, 1, out)
        self.assertIn(f"{apt.OFL}/filament/Stray.json: no {apt.OFL}.json list "
                      f"references it", out)

    def test_a_duplicate_key_is_an_error(self):
        self.bundle().write_raw("V", "filament/B.json",
                                b'{"type":"filament","name":"B","name":"B2"}')
        errors, out = self.per_vendor_errors()
        self.assertGreater(errors, 0)
        self.assertIn("Duplicate key", out)

    def test_the_index_and_the_file_must_agree_on_the_name(self):
        self.bundle()
        self.t.write("V", "filament/C.json", {"type": "filament", "name": "Other"})
        self.t.index("V", "filament", "C", "filament/C.json")
        errors, out = self.per_vendor_errors()
        self.assertGreater(errors, 0)
        self.assertIn("name mismatch", out)

    def test_an_index_entry_with_no_file_is_an_error(self):
        self.bundle()
        self.t.index("V", "filament", "Gone", "filament/Gone.json")
        errors, out = self.per_vendor_errors()
        self.assertGreater(errors, 0)
        self.assertIn("Missing sub profile", out)

    def test_a_vector_option_may_not_be_a_scalar(self):
        self.bundle().write("V", "filament/B.json", {
            "type": "filament", "name": "B", "filament_type": "PLA"})
        errors, out = self.per_vendor_errors()
        self.assertGreater(errors, 0)
        self.assertIn("must be an array", out)

    def test_renamed_and_old_option_may_not_co_exist(self):
        self.bundle().write("V", "machine/M.json", {
            "type": "machine", "name": "M 0.4 nozzle",
            "extruder_clearance_radius": "45", "extruder_clearance_max_radius": "68"})
        errors, out = self.per_vendor_errors()
        self.assertGreater(errors, 0)
        self.assertIn("Conflict keys", out)

    def test_the_length_rule_only_binds_indexed_presets(self):
        # A file the index never loads cannot break AMS matching, and some
        # bundles ship such orphans from before the rule existed.
        self.bundle().write("V", "filament/Long.json", {
            "type": "filament", "name": "Long", "filament_id": "OFtoolongforams"})
        errors, out = self.per_vendor_errors()
        self.assertEqual(errors, 0, out)
        self.t.index("V", "filament", "Long", "filament/Long.json")
        errors, out = self.per_vendor_errors()
        self.assertGreater(errors, 0)
        self.assertIn("Filament id too long", out)

    def test_obsolete_key_warnings_exclude_active_and_renamed_options(self):
        self.bundle().write("V", "filament/B.json", {
            "type": "filament", "name": "B", "silent_mode": "0",
            "anisotropic_surfaces": "0", "extruder_type": ["Direct Drive"],
            "extruder_clearance_max_radius": "68"})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            warnings = apt.check_obsolete_keys(self.t.profiles, "V")
        self.assertEqual(warnings, 2)
        self.assertIn("Obsolete key", buf.getvalue())

    def test_obsolete_key_warnings_run_without_a_flag(self):
        self.t.write("V", "filament/A.json", {
            "type": "filament", "name": "A", "silent_mode": "0"})
        self.run_command("update-index")
        snapshot = os.path.join(self.t.dir, "snapshot.json")
        self.run_command("update-snapshot", "--snapshot", snapshot)
        rc, out = self.run_command("check", "--snapshot", snapshot)
        self.assertEqual(rc, 1, out)  # normalization also rejects the obsolete key
        self.assertIn("Obsolete key: 'silent_mode' found in V/filament/A.json", out)
        self.assertIn("Files with warnings : 1", out)

    def test_a_default_material_must_exist_somewhere(self):
        self.bundle().write("V", "machine/M.json", {
            "type": "machine", "name": "M 0.4 nozzle",
            "default_materials": "A;Nope"})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors, _warn = apt.check_machine_default_materials(self.t.profiles, "V")
        self.assertEqual(errors, 1)
        self.assertIn("'Nope'", buf.getvalue())

    def test_a_default_material_fails_check_without_a_flag(self):
        # The reference check is part of the default run, not an opt-in: a
        # dangling name has to fail plain `check`.
        self.bundle()
        self.t.write("V", "machine/M.json", {
            "type": "machine", "name": "M 0.4 nozzle",
            "default_filament_profile": ["A", "Nope"]})
        self.t.index("V", "machine", "M 0.4 nozzle", "machine/M.json")
        snapshot = os.path.join(self.t.dir, "snapshot.json")
        self.run_command("update-snapshot", "--snapshot", snapshot)
        rc, out = self.run_command("check", "--snapshot", snapshot)
        self.assertEqual(rc, 1, out)
        self.assertIn("Missing filament profile: 'Nope'", out)

    def test_the_stray_user_directory_is_not_a_vendor(self):
        # A local validator run leaves resources/profiles/user/ behind; an
        # unscoped check must not count it as a bundle and warn about it.
        self.bundle()
        for sub in apt.PROFILE_SUBDIRS:
            os.makedirs(os.path.join(self.t.profiles, apt.USER_DIR, "default", sub))
        snapshot = os.path.join(self.t.dir, "snapshot.json")
        self.run_command("update-snapshot", "--snapshot", snapshot)
        _rc, out = self.run_command("check", "--snapshot", snapshot)
        self.assertIn("Checked vendors     : 1", out)
        self.assertNotIn("user", out)

    def names(self, vendor="V"):
        """The preset name check for one bundle, which is what --vendor narrows."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = apt.check_preset_name_uniqueness(self.t.profiles, vendor)
        return errors, buf.getvalue()

    def test_one_bundle_may_not_hold_two_profiles_of_a_name(self):
        self.bundle().write("V", "filament/dup.json", {
            "type": "filament", "name": "A", "instantiation": "false"})
        errors, out = self.names()
        self.assertEqual(errors, 1, out)
        self.assertIn('V has 2 filament profiles named "A"', out)

    def test_an_unindexed_twin_counts_as_a_duplicate(self):
        # The case this check was written for: a stale copy of a base profile in
        # machine/, which no per-vendor check walked, one index edit away from
        # silently deciding which of the two a whole bundle inherits from.
        self.bundle()
        self.t.write("V", "machine/HSN/base.json",
                     {"type": "machine", "name": "V base"})
        self.t.index("V", "machine", "V base", "machine/HSN/base.json")
        self.t.write("V", "machine/base.json", {"type": "machine", "name": "V base"})
        errors, out = self.names()
        self.assertEqual(errors, 1, out)
        self.assertIn("machine/HSN/base.json", out)
        self.assertIn("machine/base.json", out)

    def test_one_name_in_two_types_is_not_a_clash(self):
        self.bundle()
        self.t.write("V", "process/same.json", {"type": "process", "name": "A"})
        errors, out = self.names()
        self.assertEqual(errors, 0, out)

    def test_a_name_is_per_bundle_not_global(self):
        # fdm_machine_common exists in 60 shipped bundles; the name is scoped to the
        # bundle that resolves it, so sharing one across vendors is not a clash.
        for vendor in ("V", "W"):
            self.t.write(vendor, "machine/common.json",
                         {"type": "machine", "name": "fdm_machine_common"})
            self.t.index(vendor, "machine", "fdm_machine_common", "machine/common.json")
        for vendor in ("V", "W"):
            errors, out = self.names(vendor)
            self.assertEqual(errors, 0, out)

    def coverage(self, vendor="V"):
        """The index-coverage check for one bundle: (errors, gaps, output)."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors, gaps = apt.check_index_coverage(self.t.profiles, vendor)
        return errors, gaps, buf.getvalue()

    def test_a_file_no_list_references_is_an_error(self):
        self.bundle().write("V", "filament/Stray.json",
                            {"type": "filament", "name": "Stray"})
        errors, gaps, out = self.coverage()
        self.assertEqual(errors, 1, out)
        self.assertEqual(gaps["unindexed"], 1)
        self.assertIn("no V.json list references it", out)

    def test_a_file_with_no_type_is_its_own_category(self):
        # update-index cannot place it, so "add it to the index" is not the remedy.
        self.bundle().write("V", "filament/Stray.json", {"name": "Stray"})
        errors, gaps, out = self.coverage()
        self.assertEqual(errors, 1, out)
        self.assertEqual(gaps["unindexable"], 1)
        self.assertIn("declares no profile type", out)

    def test_an_unparsable_unlisted_file_is_reported_too(self):
        self.bundle().write_raw("V", "filament/Broken.json", b"{ not json")
        errors, gaps, out = self.coverage()
        self.assertEqual(errors, 1, out)
        self.assertEqual(gaps["unindexable"], 1)

    def test_a_dotted_sub_path_still_counts_as_listed(self):
        self.bundle()
        self.t.write("V", "filament/Dotted.json",
                     {"type": "filament", "name": "Dotted"})
        self.t.index("V", "filament", "Dotted", "filament/./Dotted.json")
        errors, _gaps, out = self.coverage()
        self.assertEqual(errors, 0, out)

    def test_a_data_file_is_not_expected_in_the_index(self):
        self.bundle().write("V", "filament/filaments_color_codes.json",
                            {"data": [], "total": 0})
        errors, _gaps, out = self.coverage()
        self.assertEqual(errors, 0, out)

    def test_a_bundle_with_no_index_is_left_to_the_name_check(self):
        # Every file unlisted because there is no list at all is one problem, not
        # one per file; check_name_consistency reports the missing index.
        self.t.write("W", "filament/A.json", {"type": "filament", "name": "A"})
        os.remove(self.t.index_path("W"))
        errors, _gaps, out = self.coverage("W")
        self.assertEqual(errors, 0, out)

    def test_the_remedy_is_printed_once_not_once_per_file(self):
        self.bundle()
        for n in range(5):
            self.t.write("V", f"filament/Stray{n}.json",
                         {"type": "filament", "name": f"Stray{n}"})
        self.t.write("V", "filament/NoType.json", {"name": "NoType"})
        snapshot = os.path.join(self.t.dir, "snapshot.json")
        self.run_command("update-snapshot", "--snapshot", snapshot)
        rc, out = self.run_command("check", "--snapshot", snapshot)
        self.assertEqual(rc, 1, out)
        self.assertEqual(out.count("update-index\" to add them"), 1, out)
        self.assertEqual(out.count("or delete them"), 1, out)
        self.assertIn("5 unreferenced file(s)", out)
        self.assertIn("1 unreferenced file(s)", out)

    def test_setting_id_uniqueness_is_tree_wide(self):
        # Two presets sharing vendor/type/name mint one id, so the collision
        # only shows up in a pass that has seen the whole tree.
        shared = apt.generate_preset_setting_id("V", "filament", "A")
        for rel in ("filament/A.json", "filament/nested/A.json"):
            self.t.write("V", rel, {"type": "filament", "name": "A",
                                    "instantiation": "true", "setting_id": shared})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = apt.check_setting_id_uniqueness(self.t.profiles)
        self.assertGreater(errors, 0)
        self.assertIn("globally unique", buf.getvalue())

    def test_a_base_profile_must_not_carry_a_setting_id(self):
        self.t.write("V", "filament/base.json", {
            "type": "filament", "name": "base", "instantiation": "false",
            "setting_id": apt.generate_preset_setting_id("V", "filament", "base")})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = apt.check_setting_id_uniqueness(self.t.profiles)
        self.assertEqual(errors, 1)
        self.assertIn("must not have a", buf.getvalue())


# ---------------------------------------------------------------------------
# check: normalize and update-index would change nothing
# ---------------------------------------------------------------------------

class TestNormalized(TreeCase):
    """The pass that holds a bundle to the shape normalize and update-index write."""

    def normalize(self):
        """Put the tree in that shape, the way a contributor is told to."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            apt.main(["normalize", "--profiles", self.t.profiles])
            apt.main(["update-index", "--profiles", self.t.profiles])
        return buf.getvalue()

    def normalized(self, vendor="V"):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors, gaps = apt.check_normalized(self.t.profiles, vendor)
        return errors, gaps, buf.getvalue()

    def snapshot(self):
        path = os.path.join(self.t.dir, "snapshot.json")
        self.run_command("update-snapshot", "--snapshot", path)
        return path

    def test_a_bundle_the_two_commands_just_wrote_reports_nothing(self):
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        self.t.write("V", "process/B.json", {"type": "process", "name": "B"})
        self.normalize()
        errors, _gaps, out = self.normalized()
        self.assertEqual(errors, 0, out)

    def test_a_profile_fix_would_rewrite_is_an_error(self):
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        self.normalize()
        # version belongs to the bundle, in <vendor>.json, never to a preset.
        data = self.t.read("V", "filament/A.json")
        data["version"] = "01.00.00.00"
        self.t.write("V", "filament/A.json", data)
        errors, gaps, out = self.normalized()
        self.assertEqual(errors, 1, out)
        self.assertEqual(gaps["unnormalized"], 1, out)
        self.assertIn("V/filament/A.json: normalize would remove version", out)

    def test_an_index_update_index_would_rebuild_is_an_error(self):
        for name, parent in (("B", "A"), ("A", None)):
            data = {"type": "filament", "name": name}
            if parent:
                data["inherits"] = parent
            self.t.write("V", f"filament/{name}.json", data)
        self.normalize()
        # Parents-first is what lets the loader resolve inherits in one pass; a
        # hand-edited list that puts the child first still names every file.
        index = self.t.read_index("V")
        index["filament_list"].reverse()
        self.t.write_index("V", index)
        errors, gaps, out = self.normalized()
        self.assertEqual(errors, 1, out)
        self.assertEqual(gaps["stale_index"], 1, out)
        self.assertIn("V.json: update-index would rebuild filament_list", out)

    def test_an_unbuildable_index_is_left_to_the_checks_that_name_it(self):
        # update-index refuses to rebuild a bundle where two files claim one name,
        # so "would be rebuilt" on top of the duplicate-name error would be noise.
        self.t.write("V", "machine/base.json", {"type": "machine", "name": "base"})
        self.normalize()
        self.t.write("V", "machine/HSN/base.json", {"type": "machine", "name": "base"})
        errors, gaps, out = self.normalized()
        self.assertEqual(errors, 0, out)
        self.assertEqual(gaps["stale_index"], 0, out)

    def test_a_bundle_with_no_index_still_has_its_files_checked(self):
        self.t.write("V", "filament/A.json",
                     {"type": "filament", "name": "A", "is_custom_defined": "0"})
        os.remove(self.t.index_path("V"))
        errors, gaps, out = self.normalized()
        self.assertEqual(errors, 1, out)
        self.assertEqual(gaps["unnormalized"], 1, out)
        self.assertEqual(gaps["stale_index"], 0, out)

    def test_the_shared_base_bundle_is_covered_too(self):
        # normalize and update-index own the shape of every bundle, the shared
        # library included.
        self.t.write(apt.OFL, "filament/A.json",
                     {"type": "filament", "name": "A", "version": "01.00.00.00"})
        rc, out = self.run_command("check", "--snapshot", self.snapshot())
        self.assertEqual(rc, 1, out)
        self.assertIn(f"{apt.OFL}/filament/A.json: normalize would remove version", out)

    def test_each_remedy_is_printed_once_for_the_whole_run(self):
        for n in range(3):
            self.t.write("V", f"filament/A{n}.json",
                         {"type": "filament", "name": f"A{n}",
                          "version": "01.00.00.00"})
        self.t.write("W", "filament/B.json", {"type": "filament", "name": "B"})
        rc, out = self.run_command("check", "--snapshot", self.snapshot())
        self.assertEqual(rc, 1, out)
        self.assertIn("3 profile file(s) above are not what", out)
        self.assertEqual(out.count('normalize" writes: run it and commit'), 1, out)
        self.assertIn("2 vendor index(es) above are not what", out)
        self.assertEqual(out.count('update-index" writes: run it and commit'), 1, out)


# ---------------------------------------------------------------------------
# CLI dispatch
# ---------------------------------------------------------------------------

class TestDispatch(TreeCase):
    def test_each_command_reaches_its_own_writer(self):
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        for command, expected in (("normalize", "normalized"),
                                  ("trim", "unreferenced"),
                                  ("update-index", "vendor index")):
            with self.subTest(command=command):
                rc, out = self.run_command(command, "--dry-run")
                self.assertEqual(rc, 0, out)
                self.assertIn(expected, out)

    def test_an_option_belongs_to_one_command_only(self):
        for argv in (["trim", "--force"],
                     ["update-index", "--filament-id"],
                     ["check", "--profile-type", "filament"],
                     ["update-snapshot", "--vendor", "V"]):
            with self.subTest(argv=argv):
                with self.assertRaises(SystemExit) as cm, \
                        contextlib.redirect_stdout(io.StringIO()), \
                        contextlib.redirect_stderr(io.StringIO()):
                    apt.main([*argv, "--profiles", self.t.profiles])
                self.assertEqual(cm.exception.code, 2)

    def test_an_unknown_vendor_stops_the_run(self):
        self.t.write("V", "filament/A.json", {"name": "A"})
        before = self.t.bytes_map()
        rc, out = self.run_command("normalize", "--vendor", "Nope")
        self.assertEqual(rc, 1)
        self.assertIn("unknown vendor", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_an_empty_vendor_means_every_vendor(self):
        self.t.write("V", "filament/A.json", {"name": "A"})
        rc, out = self.run_command("normalize", "--vendor", "")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.t.read("V", "filament/A.json")["type"], "filament")


# ---------------------------------------------------------------------------
# the real tree
# ---------------------------------------------------------------------------

@unittest.skipUnless(os.path.isdir(REAL_PROFILES), "resources/profiles not present")
class TestRealTree(unittest.TestCase):
    def test_check_passes(self):
        # The exact CI invocation, return code included.
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = apt.main(["check"])
        self.assertEqual(rc, 0, buf.getvalue())

    def test_the_shipped_tree_needs_no_fix(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            changed, errors = apt.normalize_profiles(REAL_PROFILES, dry_run=True)
        self.assertEqual(errors, 0, buf.getvalue())
        self.assertEqual(changed, 0, buf.getvalue())

    def test_the_shipped_indexes_need_no_rebuild(self):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            changed, errors = apt.update_profile_indexes(REAL_PROFILES, dry_run=True)
        self.assertEqual(errors, 0, buf.getvalue())
        self.assertEqual(changed, 0, buf.getvalue())

    def test_no_shipped_bundle_is_a_stray_json_file(self):
        # blacklist.json has no directory beside it, so it is not a vendor.
        self.assertNotIn("blacklist", apt.list_vendor_names(REAL_PROFILES))


if __name__ == "__main__":
    unittest.main()
