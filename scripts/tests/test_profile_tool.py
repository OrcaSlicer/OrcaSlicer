#!/usr/bin/env python3
"""Tests for the tree-maintenance half of scripts/orca_profile_tool.py: the
normalize, trim, update-index, fix-variant, check and gcode-diff commands, and the
subcommand dispatch that reaches them (stdlib unittest, no external deps).

The id halves are covered by test_filament_id.py and test_setting_id.py.

Run from the repo root:  python -m unittest discover -s scripts/tests -v
"""

import contextlib
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

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
        root = os.path.abspath(self.dir)

        def writable_retry(func, path, _exc):
            # Windows refuses to delete read-only files such as git's objects, and POSIX
            # refuses to delete the entries of a read-only directory.
            if not os.path.lexists(path):  # already removed, as on a second cleanup
                return
            parent = os.path.dirname(os.path.abspath(path))
            try:
                if os.path.commonpath([parent, root]) == root:  # never the temp dir itself
                    os.chmod(parent, stat.S_IRWXU)
                if not os.path.islink(path):  # chmod would follow it out of the tree
                    os.chmod(path, stat.S_IRWXU)
                func(path)
            except (OSError, ValueError):  # commonpath raises ValueError across drives
                pass
        hook = "onexc" if sys.version_info >= (3, 12) else "onerror"
        shutil.rmtree(self.dir, **{hook: writable_retry})

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


class TestVariantScheme(unittest.TestCase):
    def test_the_key_sets_are_read_from_the_engine(self):
        scheme = apt._variant_scheme()
        self.assertEqual(scheme["machine"][1]["retraction_length"], 1)
        self.assertEqual(scheme["machine"][1]["machine_max_speed_x"], 2)
        self.assertIn("outer_wall_speed", scheme["process"][1])
        self.assertIn("nozzle_temperature", scheme["filament"][1])
        # Commented out of its initializer, so not a member.
        self.assertNotIn("filament_extruder_id", scheme["filament"][1])
        for list_key, strides in scheme.values():
            self.assertEqual(strides[list_key], 1)

    def test_the_variant_names_are_read_from_the_engine(self):
        extruder_types, volume_types, legacy, extruder_legacy = apt._variant_names()
        self.assertEqual(extruder_types, {"Direct Drive", "Bowden"})
        self.assertEqual(volume_types, {"Standard", "High Flow", "TPU High Flow", "E3D High Flow", "Extra High Flow"})
        # Hybrid is an enum value no variant string may name.
        self.assertNotIn("Hybrid", volume_types)
        self.assertEqual(legacy, {"Normal": "Standard", "Big Traffic": "High Flow"})
        self.assertEqual(extruder_legacy, {"DirectDrive": "Direct Drive"})


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

    def test_include_targets_are_listed_before_their_users(self):
        # The loader resolves include like inherits: in one pass over the list, so
        # a template must be listed before every preset that includes it - even
        # though a template has no parent of its own to order it by.
        self.t.write("V", "machine/P.json", {"type": "machine", "name": "P",
                                             "include": ["T start", "T end"]})
        self.t.write("V", "machine/T start.json", {"type": "machine", "name": "T start"})
        self.t.write("V", "machine/T end.json", {"type": "machine", "name": "T end"})
        self.t.write("V", "filament/F.json", {"type": "filament", "name": "F",
                                              "inherits": "B", "include": "S"})
        self.t.write("V", "filament/B.json", {"type": "filament", "name": "B"})
        self.t.write("V", "filament/S.json", {"type": "filament", "name": "S"})
        rc, out = self.run_command("update-index")
        self.assertEqual(rc, 0, out)
        machines = [e["name"] for e in self.t.read_index("V")["machine_list"]]
        self.assertEqual(machines, ["T end", "T start", "P"])
        filaments = [e["name"] for e in self.t.read_index("V")["filament_list"]]
        self.assertLess(filaments.index("B"), filaments.index("F"))
        self.assertLess(filaments.index("S"), filaments.index("F"))

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
        rc, out = self.run_command("check")
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
        rc, out = self.run_command("check")
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
        rc, out = self.run_command("check")
        self.assertEqual(rc, 1, out)
        self.assertIn("Missing filament profile: 'Nope'", out)

    def test_the_stray_user_directory_is_not_a_vendor(self):
        # A local validator run leaves resources/profiles/user/ behind; an
        # unscoped check must not count it as a bundle and warn about it.
        self.bundle()
        for sub in apt.PROFILE_SUBDIRS:
            os.makedirs(os.path.join(self.t.profiles, apt.USER_DIR, "default", sub))
        _rc, out = self.run_command("check")
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

    def machine_models(self):
        """The cross-vendor machine_model name check, whole tree by design."""
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = apt.check_machine_model_name_uniqueness(self.t.profiles)
        return errors, buf.getvalue()

    def test_two_bundles_may_not_declare_one_machine_model_name(self):
        # The name keys the global printer-type lookup: Preset::get_printer_type
        # matches a preset's printer_model against every vendor's model names, so a
        # copy of another vendor's model is ambiguous, not merely duplicated.
        for vendor in ("V", "W"):
            self.t.write(vendor, "machine/MyKlipper.json",
                         {"type": "machine_model", "name": "Generic Klipper Printer",
                          "model_id": "my_klipper_01"})
        errors, out = self.machine_models()
        self.assertEqual(errors, 1, out)
        self.assertIn('machine_model name "Generic Klipper Printer"', out)
        self.assertIn("V/machine/MyKlipper.json", out)
        self.assertIn("W/machine/MyKlipper.json", out)

    def test_distinct_machine_model_names_are_left_alone(self):
        for vendor in ("V", "W"):
            self.t.write(vendor, "machine/model.json",
                         {"type": "machine_model", "name": f"{vendor} Model"})
        errors, out = self.machine_models()
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
        rc, out = self.run_command("check")
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
        rc, out = self.run_command("check")
        self.assertEqual(rc, 1, out)
        self.assertIn(f"{apt.OFL}/filament/A.json: normalize would remove version", out)

    def test_each_remedy_is_printed_once_for_the_whole_run(self):
        for n in range(3):
            self.t.write("V", f"filament/A{n}.json",
                         {"type": "filament", "name": f"A{n}",
                          "version": "01.00.00.00"})
        self.t.write("W", "filament/B.json", {"type": "filament", "name": "B"})
        rc, out = self.run_command("check")
        self.assertEqual(rc, 1, out)
        self.assertIn("3 profile file(s) above are not what", out)
        self.assertEqual(out.count('normalize" writes: run it and commit'), 1, out)
        self.assertIn("2 vendor index(es) above are not what", out)
        self.assertEqual(out.count('update-index" writes: run it and commit'), 1, out)


# ---------------------------------------------------------------------------
# CLI dispatch
# ---------------------------------------------------------------------------

class TestFixVariant(TreeCase):
    def test_cooling_arrays_require_one_value_per_declared_variant(self):
        keys = ("fan_min_speed", "fan_max_speed", "additional_cooling_fan_speed")
        variants = ["Direct Drive Standard", "Direct Drive High Flow", "Bowden Standard"]
        for width in (1, 2, 3, 4):
            with self.subTest(width=width):
                self.preset("filament/F.json", instantiation="true",
                            filament_extruder_variant=variants,
                            **{key: ["20"] * width for key in keys})
                apt.load_vendor_configs.cache_clear()
                errors, out = self.width_errors()
                self.assertEqual(errors, 0 if width == 3 else len(keys), out)
                if width != 3:
                    for key in keys:
                        self.assertIn(f'"{key}" has {width} values', out)

    def preset(self, rel, **data):
        name = os.path.splitext(os.path.basename(rel))[0]
        self.t.write("V", rel, {"type": rel.split("/")[0], "name": name, **data})
        self.t.index("V", rel.split("/")[0], name, rel)

    def processes(self, common):
        """outer_wall_speed written once in a list-less base, reached by list-less
        presets through one base and by two-variant presets through another."""
        dual = ["Direct Drive Standard", "Direct Drive High Flow"]
        self.preset("process/common.json", instantiation="false", outer_wall_speed=common)
        self.preset("process/single.json", inherits="common", instantiation="false")
        self.preset("process/dual.json", inherits="common", instantiation="false",
                    print_extruder_variant=dual, print_extruder_id=["1", "1"])
        for name, base in (("S1", "single"), ("S2", "single"), ("D1", "dual")):
            self.preset(f"process/{name}.json", inherits=base, instantiation="true")

    def width_errors(self, *strict):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors, _warnings = apt.check_variant_arrays(self.t.profiles, "V", bool(strict))
        return errors, buf.getvalue()

    def test_a_machine_without_a_variant_list_has_one_variant_per_extruder(self):
        # extruder_variant_list defaults to one "Direct Drive Standard" per extruder.
        self.preset("machine/M.json", instantiation="true", nozzle_diameter=["0.4", "0.4"],
                    retraction_length=["0.8", "1.2"], z_hop=["0.4"],
                    machine_max_speed_x=["500", "200"])
        errors, out = self.width_errors()
        self.assertEqual(errors, 2, out)
        self.assertIn('M.json: "z_hop" has 1 values for variant length 2 (no '
                      'printer_extruder_variant, so one default variant per extruder) at stride 1, '
                      'which takes 2', out)
        self.assertIn('M.json: "machine_max_speed_x" has 2 values for variant length 2', out)
        self.assertNotIn("retraction_length", out)

    def test_one_value_is_an_error_where_the_list_has_more_variants(self):
        self.preset("process/P.json", instantiation="true", outer_wall_speed=["30"],
                    print_extruder_id=["1", "1"],
                    print_extruder_variant=["Direct Drive Standard", "Direct Drive High Flow"])
        errors, out = self.width_errors()
        self.assertEqual(errors, 1, out)
        self.assertIn('P.json: "outer_wall_speed" has 1 values for variant length 2 at stride 1, '
                      'which takes 2', out)

    def test_one_value_is_padded_to_every_variant(self):
        self.preset("machine/M.json", instantiation="true", nozzle_diameter=["0.4"] * 3,
                    retraction_length=["0.8"], machine_max_speed_x=["500", "200"])
        rc, out = self.run_command("fix-variant")
        self.assertEqual(rc, 0, out)
        machine = self.t.read("V", "machine/M.json")
        self.assertEqual(machine["retraction_length"], ["0.8"] * 3)
        self.assertEqual(machine["machine_max_speed_x"], ["500", "200"] * 3)
        self.assertEqual(self.width_errors()[0], 0)

    def test_extruder_ids_are_left_to_check(self):
        self.preset("process/P.json", instantiation="true", print_extruder_id=["1"],
                    print_extruder_variant=["Direct Drive Standard", "Direct Drive High Flow"])
        self.run_command("fix-variant")
        self.assertEqual(self.t.read("V", "process/P.json")["print_extruder_id"], ["1"])
        errors, out = self.width_errors()
        self.assertEqual(errors, 1, out)
        self.assertIn("print_extruder_id has 1 entries for the 2 entries", out)

    def test_a_base_is_judged_only_where_its_array_reaches_a_preset(self):
        # A leaf overriding the base's one-value limit passes in both modes.
        self.preset("machine/common.json", instantiation="false", machine_max_speed_x=["500"])
        self.preset("machine/M.json", inherits="common", instantiation="true",
                    machine_max_speed_x=["500", "200"])
        self.assertEqual(self.width_errors()[0], 0)
        self.assertEqual(self.width_errors("strict")[0], 0)

    def test_a_shared_array_is_fixed_in_the_base_whose_presets_agree(self):
        self.processes(["30", "40"])
        self.assertEqual(self.width_errors()[0], 0)
        errors, out = self.width_errors("strict")
        self.assertEqual(errors, 2, out)
        self.assertIn('process/S1.json: "outer_wall_speed" has 2 values for variant length 1', out)
        rc, out = self.run_command("fix-variant", "--strict")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.t.read("V", "process/single.json")["outer_wall_speed"], ["30"])
        self.assertEqual(self.t.read("V", "process/common.json")["outer_wall_speed"],
                         ["30", "40"])
        self.assertNotIn("outer_wall_speed", self.t.read("V", "process/S1.json"))
        self.assertEqual(self.width_errors("strict")[0], 0)

    def test_an_inherited_array_is_checked_and_restated_only_when_strict(self):
        self.processes(["30"])
        self.assertEqual(self.width_errors()[0], 0)
        errors, out = self.width_errors("strict")
        self.assertEqual(errors, 1, out)
        self.assertIn('process/D1.json: "outer_wall_speed" has 1 values for variant length 2 at '
                      'stride 1, which takes 2 (it comes from V/process/common.json)', out)
        before = self.t.bytes_map()
        self.run_command("fix-variant")
        self.assertEqual(self.t.bytes_map(), before)
        rc, out = self.run_command("fix-variant", "--strict")
        self.assertEqual(rc, 0, out)
        self.assertEqual(self.t.read("V", "process/dual.json")["outer_wall_speed"], ["30", "30"])
        self.assertEqual(self.t.read("V", "process/common.json")["outer_wall_speed"], ["30"])
        self.assertEqual(self.width_errors("strict")[0], 0)

    def test_a_strict_array_never_lands_in_a_file_of_another_width(self):
        # common is list-less (width 1) and D2 inherits it directly, so D2 takes it.
        self.processes(["30"])
        self.preset("process/D2.json", inherits="common", instantiation="true",
                    print_extruder_variant=["Direct Drive Standard", "Direct Drive High Flow"],
                    print_extruder_id=["1", "1"])
        self.run_command("fix-variant", "--strict")
        self.assertEqual(self.t.read("V", "process/D2.json")["outer_wall_speed"], ["30", "30"])
        self.assertEqual(self.t.read("V", "process/common.json")["outer_wall_speed"], ["30"])
        self.assertEqual(self.width_errors("strict")[0], 0)

    def test_extra_values_are_dropped_where_they_are_written(self):
        self.preset("process/base.json", instantiation="false", outer_wall_speed=["30", "0"])
        self.preset("process/P1.json", inherits="base", instantiation="true",
                    inner_wall_speed=["40", "0"])
        self.preset("process/P2.json", inherits="base", instantiation="true")
        self.run_command("fix-variant")
        self.assertEqual(self.t.read("V", "process/P1.json")["inner_wall_speed"], ["40"])
        self.assertEqual(self.t.read("V", "process/base.json")["outer_wall_speed"], ["30", "0"])
        self.run_command("fix-variant", "--strict")
        self.assertEqual(self.t.read("V", "process/base.json")["outer_wall_speed"], ["30"])
        self.assertNotIn("outer_wall_speed", self.t.read("V", "process/P1.json"))

    def test_the_rest_of_the_file_keeps_its_bytes(self):
        self.t.write_raw("V", "process/P.json", b'{\n  "type": "process", "name": "P",\n'
                         b'  "instantiation": "true",\n  "outer_wall_speed": ["30", "0"],\n'
                         b'  "inner_wall_speed": [\n    "40",\n    "0"\n  ]\n}\n')
        self.t.index("V", "process", "P", "process/P.json")
        self.run_command("fix-variant")
        self.assertEqual(self.t.raw("V", "process/P.json"),
                         b'{\n  "type": "process", "name": "P",\n'
                         b'  "instantiation": "true",\n  "outer_wall_speed": ["30"],\n'
                         b'  "inner_wall_speed": [\n    "40"\n  ]\n}\n')

    def test_a_dry_run_writes_nothing(self):
        self.processes(["30"])
        before = self.t.bytes_map()
        rc, out = self.run_command("fix-variant", "--strict", "--dry-run")
        self.assertEqual(rc, 0, out)
        self.assertIn("would resize V/process/dual.json: outer_wall_speed added -> 2", out)
        self.assertEqual(self.t.bytes_map(), before)

    def test_padding_repeats_the_last_value_or_pair(self):
        self.assertEqual(apt._fit_width(["a", "b"], 4, 1), ["a", "b", "b", "b"])
        self.assertEqual(apt._fit_width(["a", "b", "c"], 6, 2), ["a", "b"] * 3)
        self.assertEqual(apt._fit_width(["a", "b", "c"], 2, 2), ["a", "b"])
        self.assertEqual(apt._fit_width(["a"], 4, 2), ["a"] * 4)


class TestVariantNames(TreeCase):
    def preset(self, rel, vendor="V", **data):
        section = rel.split("/")[0]
        name = os.path.splitext(os.path.basename(rel))[0]
        self.t.write(vendor, rel, {"type": section, "name": name, **data})
        self.t.index(vendor, section, name, rel)

    def names(self, vendor="V"):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            errors = apt.check_variant_names(self.t.profiles, vendor)
        return errors, buf.getvalue()

    def test_a_legal_variant_layout_passes(self):
        self.preset("machine/M.json", instantiation="true", nozzle_diameter=["0.4"],
                    extruder_type=["Direct Drive"],
                    extruder_variant_list=["Direct Drive Standard,Direct Drive High Flow"],
                    printer_extruder_id=["1", "1"],
                    printer_extruder_variant=["Direct Drive Standard",
                                              "Direct Drive High Flow"],
                    nozzle_volume_type=["High Flow"],
                    default_nozzle_volume_type=["Standard"])
        self.preset("process/P.json", instantiation="true", print_extruder_id=["1", "1"],
                    print_extruder_variant=["Direct Drive Standard",
                                            "Direct Drive High Flow"])
        self.preset("filament/F.json", instantiation="true",
                    filament_extruder_variant=["Direct Drive Standard", "Bowden High Flow"])
        self.assertEqual(self.names()[0], 0, self.names()[1])

    def test_a_variant_the_enum_cannot_build_is_an_error(self):
        # An unknown volume type, in a list and in a menu, an extruder type that is not
        # one, and the legacy extruder spelling that is rewritten in extruder_type alone.
        cases = (("process/P1.json", "print_extruder_variant", "Direct Drive Fast",
                  'entry 1 holds "Direct Drive Fast"',
                  "not a nozzle volume type the enum has"),
                 ("machine/M.json", "extruder_variant_list",
                  "Direct Drive Standard,Direct Drive Fast",
                  'extruder 1 holds "Direct Drive Fast"',
                  "not a nozzle volume type the enum has"),
                 ("process/P2.json", "print_extruder_variant", "DD Standard",
                  'entry 1 holds "DD Standard"',
                  "does not start with an extruder type the enum has"),
                 ("filament/F.json", "filament_extruder_variant", "DirectDrive Standard",
                  'entry 1 holds "DirectDrive Standard"',
                  "does not start with an extruder type the enum has"))
        for rel, key, value, _held, _why in cases:
            self.preset(rel, instantiation="false", **{key: [value]})
        errors, out = self.names()
        self.assertEqual(errors, len(cases), out)
        for rel, key, _value, held, why in cases:
            self.assertIn(f"V/{rel}: {key} {held}", out)
            self.assertIn(why, out)

    def test_the_legacy_names_the_loader_rewrites_are_errors(self):
        self.preset("machine/M.json", extruder_type=["DirectDrive"],
                    nozzle_volume_type=["Big Traffic"],
                    default_nozzle_volume_type=["Normal"],
                    printer_extruder_variant=["Direct Drive Big Traffic"])
        errors, out = self.names()
        self.assertEqual(errors, 4, out)
        self.assertIn('extruder_type spells the legacy name "DirectDrive"; the loader '
                      'still rewrites it to "Direct Drive"', out)
        self.assertIn('V/machine/M.json: nozzle_volume_type spells the legacy name '
                      '"Big Traffic"', out)
        self.assertIn('default_nozzle_volume_type spells the legacy name "Normal"', out)
        self.assertIn('holds the legacy variant "Direct Drive Big Traffic"', out)
        self.assertIn('a profile writes the enum name: "Direct Drive High Flow"', out)

    def test_an_empty_entry_is_an_error(self):
        self.preset("filament/F.json",
                    filament_extruder_variant=["Direct Drive Standard", ""])
        errors, out = self.names()
        self.assertEqual(errors, 1, out)
        self.assertIn("filament_extruder_variant entry 2 is empty", out)

    def test_hybrid_is_runtime_only(self):
        self.preset("machine/M.json", nozzle_volume_type=["Hybrid"],
                    default_nozzle_volume_type=["Hybrid"],
                    printer_extruder_variant=["Direct Drive Hybrid"])
        errors, out = self.names()
        self.assertEqual(errors, 3, out)
        self.assertIn("Hybrid names the sub-nozzles of one hybrid extruder at runtime", out)
        self.assertIn('V/machine/M.json: nozzle_volume_type names "Hybrid"', out)
        self.assertIn('default_nozzle_volume_type names "Hybrid"', out)

    def test_bbl_is_held_to_the_enums_like_every_bundle(self):
        # E3D High Flow passes because the engine's enum has it; TPU285 is a name no enum has.
        self.preset("machine/M.json", vendor="BBL", extruder_type=["Direct Drive"],
                    extruder_variant_list=["Direct Drive Standard,Direct Drive E3D High Flow"],
                    printer_extruder_id=["1", "1"],
                    printer_extruder_variant=["Direct Drive Standard",
                                              "Direct Drive E3D High Flow"])
        self.preset("process/P.json", vendor="BBL", instantiation="true",
                    print_extruder_variant=["Direct Drive TPU285"])
        errors, out = self.names("BBL")
        self.assertEqual(errors, 1, out)
        self.assertNotIn('holds "Direct Drive E3D High Flow"', out)
        self.assertIn('"TPU285" is not a nozzle volume type the enum has', out)

    def test_a_list_may_not_name_one_variant_twice(self):
        self.preset("filament/F.json",
                    filament_extruder_variant=["Direct Drive Standard",
                                               "Direct Drive High Flow",
                                               "Direct Drive Standard"])
        self.preset("machine/M.json",
                    extruder_variant_list=["Direct Drive Standard,Direct Drive Standard,"
                                           "Direct Drive High Flow"])
        self.preset("process/P.json", print_extruder_id=["1", "1", "1"],
                    print_extruder_variant=["Direct Drive Standard", "Direct Drive High Flow",
                                            "Direct Drive Standard"])
        errors, out = self.names()
        self.assertEqual(errors, 3, out)
        self.assertIn('filament_extruder_variant lists "Direct Drive Standard" 2 times', out)
        self.assertIn('extruder_variant_list extruder 1 lists "Direct Drive Standard" 2 '
                      "times", out)
        self.assertIn('print_extruder_variant lists the pair (extruder 1, "Direct Drive '
                      'Standard") 2 times', out)

    def test_one_variant_on_two_extruders_is_two_pairs_not_a_repeat(self):
        self.preset("process/P.json", print_extruder_id=["1", "2"],
                    print_extruder_variant=["Direct Drive Standard",
                                            "Direct Drive Standard"])
        self.assertEqual(self.names()[0], 0, self.names()[1])

    def test_an_unknown_enum_value_fails_the_bundle(self):
        self.preset("machine/M.json", extruder_type=["Direct Drive", "Magnetic Drive"],
                    nozzle_volume_type=["Turbo"],
                    default_nozzle_volume_type=["Fast Flow"])
        errors, out = self.names()
        self.assertEqual(errors, 3, out)
        self.assertIn('extruder_type "Magnetic Drive" is not one of (Bowden, Direct Drive)',
                      out)
        self.assertIn('V/machine/M.json: nozzle_volume_type "Turbo" is not one of (E3D High '
                      "Flow, Extra High Flow, High Flow, Standard, TPU High Flow)", out)
        self.assertIn('default_nozzle_volume_type "Fast Flow" is not one of (E3D High Flow, '
                      "Extra High Flow, High Flow, Standard, TPU High Flow)", out)

    def test_a_file_no_list_references_is_not_judged(self):
        # It never loads, so its variant names cannot reach anything;
        # check_index_coverage reports the file itself.
        self.t.write("V", "machine/M.json", {"type": "machine", "name": "M",
                                             "printer_extruder_variant": ["Direct Drive Fast"]})
        self.assertEqual(self.names()[0], 0, self.names()[1])

    def test_check_runs_it(self):
        self.preset("filament/F.json", instantiation="false",
                    filament_extruder_variant=["Direct Drive Normal"])
        self.run_command("update-index")  # so nothing but the name check can fail
        rc, out = self.run_command("check")
        self.assertEqual(rc, 1, out)
        self.assertIn('holds the legacy variant "Direct Drive Normal"', out)


# ---------------------------------------------------------------------------
# legacy-compat
# ---------------------------------------------------------------------------

class TestLegacyCompat(unittest.TestCase):
    """A base release and the current tree, each with one vendor."""

    def setUp(self):
        self.base, self.cur = Tree(), Tree()
        self.addCleanup(self.base.cleanup)
        self.addCleanup(self.cur.cleanup)

    @staticmethod
    def printer(tree, name, **extra):
        tree.write("V", f"machine/{name}.json",
                   {"type": "machine", "name": name, "instantiation": "true", **extra})
        tree.index("V", "machine", name, f"machine/{name}.json")

    @staticmethod
    def filament(tree, name, printers, filament_id="OFpetg01", **extra):
        tree.write("V", f"filament/{name}.json",
                   {"type": "filament", "name": name, "instantiation": "true",
                    "filament_id": filament_id, "compatible_printers": printers, **extra})
        tree.index("V", "filament", name, f"filament/{name}.json")

    def generate(self):
        return apt.legacy_compatible_printers(self.base.profiles, self.cur.profiles)

    def test_renamed_and_narrowed_preset_keeps_printers_a_same_id_sibling_covers(self):
        for tree in (self.base, self.cur):
            for p in ("A 0.4", "A 0.6", "B 0.4"):
                self.printer(tree, p)
        self.filament(self.base, "PETG @A-all", ["A 0.4", "A 0.6", "B 0.4"])
        self.filament(self.cur, "PETG @V A-all", ["A 0.6"], renamed_from="PETG @A-all;PETG A-all")
        self.filament(self.cur, "PETG @A 0.4", ["A 0.4"])
        self.assertEqual(self.generate(), {"PETG @A-all": ["A 0.4"]})

    def test_each_old_name_gets_back_only_its_own_list(self):
        # The current "PC @0.4" existed in the base and also replaces the split "PC".
        for tree in (self.base, self.cur):
            for p in ("A 0.4", "A 0.8"):
                self.printer(tree, p)
        self.filament(self.base, "PC", ["A 0.4", "A 0.8"])
        self.filament(self.base, "PC @0.4", ["A 0.4"])
        self.filament(self.cur, "PC @0.4", ["A 0.4"], renamed_from="PC")
        self.filament(self.cur, "PC @0.8", ["A 0.8"])
        self.assertEqual(self.generate(), {"PC": ["A 0.8"]})

    def test_old_name_now_a_base_preset_resolves_through_renamed_from(self):
        for tree in (self.base, self.cur):
            for p in ("I 0.4", "Plus 0.4"):
                self.printer(tree, p)
        self.filament(self.base, "PLA @base", ["I 0.4", "Plus 0.4"])
        self.filament(self.cur, "PLA @base", [], instantiation="false")
        self.filament(self.cur, "PLA @I 0.4", ["I 0.4"], renamed_from="PLA @base")
        self.filament(self.cur, "PLA @Plus 0.4", ["Plus 0.4"])
        self.assertEqual(self.generate(), {"PLA @base": ["Plus 0.4"]})

    def test_printer_without_a_successor_is_left_out(self):
        for tree in (self.base, self.cur):
            for p in ("A 0.4", "A 0.6"):
                self.printer(tree, p)
        self.filament(self.base, "PETG @A", ["A 0.4", "A 0.6"])
        self.filament(self.cur, "PETG @A", ["A 0.6"])
        self.filament(self.cur, "Other @A", ["A 0.4"], filament_id="OFother1")
        self.assertEqual(self.generate(), {})

    def test_printer_names_follow_printer_renames(self):
        self.printer(self.base, "Old 0.4")
        self.printer(self.base, "A 0.6")
        self.printer(self.cur, "New 0.4", renamed_from="Old 0.4")
        self.printer(self.cur, "A 0.6")
        self.filament(self.base, "PETG @A", ["Old 0.4", "A 0.6"])
        self.filament(self.cur, "PETG @A", ["A 0.6"])
        self.filament(self.cur, "PETG @New", ["New 0.4"])
        self.assertEqual(self.generate(), {"PETG @A": ["New 0.4"]})

    def test_printer_without_a_shared_extruder_variant_is_left_out(self):
        for tree in (self.base, self.cur):
            self.printer(tree, "Direct 0.4", printer_extruder_variant=["Direct Drive Standard"])
            self.printer(tree, "Bowden 0.4", printer_extruder_variant=["Bowden Standard"])
        self.filament(self.base, "PETG @all", ["Direct 0.4", "Bowden 0.4"])
        self.filament(self.cur, "PETG @all", ["Direct 0.4"],
                      filament_extruder_variant=["Direct Drive Standard"])
        self.filament(self.cur, "PETG @Bowden", ["Bowden 0.4"])
        self.assertEqual(self.generate(), {})

    def test_preset_now_listing_no_printer_is_skipped(self):
        for tree in (self.base, self.cur):
            self.printer(tree, "A 0.4")
        self.filament(self.base, "PETG @A", ["A 0.4"])
        self.filament(self.cur, "PETG @A", [])
        self.assertEqual(self.generate(), {})

    def test_check_passes_on_a_current_map_and_fails_on_a_stale_one(self):
        for tree in (self.base, self.cur):
            for p in ("A 0.4", "A 0.6"):
                self.printer(tree, p)
        self.filament(self.base, "PETG @A", ["A 0.4", "A 0.6"])
        self.filament(self.cur, "PETG @A", ["A 0.6"])
        self.filament(self.cur, "PETG @A 0.4", ["A 0.4"])
        output = os.path.join(self.cur.dir, "map.json")
        argv = ["legacy-compat", "--base", self.base.profiles, "--profiles", self.cur.profiles,
                "--output", output]
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(apt.main(argv), 0)
            self.assertEqual(apt.main(argv + ["--check"]), 0)
            with open(output, "w", encoding="utf-8") as f:
                json.dump({"presets": {}}, f)
            self.assertEqual(apt.main(argv + ["--check"]), 1)


class TestDispatch(TreeCase):
    def test_each_command_reaches_its_own_writer(self):
        self.t.write("V", "filament/A.json", {"type": "filament", "name": "A"})
        for command, expected in (("normalize", "normalized"),
                                  ("fix-variant", "resized"),
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
                     ["normalize", "--strict"]):
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
# gcode-diff
# ---------------------------------------------------------------------------

@unittest.skipUnless(shutil.which("git"), "git not installed")
class TestGcodeDiff(TreeCase):
    START = "G28\nM104 S200\nG1 Z5"

    def git(self, *args):
        subprocess.run(["git", "-C", self.t.dir, "-c", "user.name=t", "-c", "user.email=t@t",
                        "-c", "commit.gpgsign=false", *args], check=True, capture_output=True)

    def printer(self, name, **extra):
        self.t.write("V", f"machine/{name}.json", {"type": "machine", "name": name, **extra})

    def commit(self):
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", "x")

    @classmethod
    def setUpClass(cls):
        # git's list of the variables that tie a process to a repository, asked with no
        # GIT_* set since a stray GIT_DIR fails the query.
        clean = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        cls.repo_env_vars = set(subprocess.run(
            ["git", "rev-parse", "--local-env-vars"], env=clean, check=True,
            capture_output=True, text=True).stdout.split())

    def setUp(self):
        super().setUp()
        # A git hook's GIT_DIR or GIT_INDEX_FILE would point every git call here, the
        # tool's included, at the real repository instead of the temp tree.
        env = {k: v for k, v in os.environ.items() if k not in self.repo_env_vars}
        patcher = mock.patch.dict(os.environ, env, clear=True)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.git("init", "-q")
        for name in ("A", "B", "C"):
            self.printer(name, machine_start_gcode=self.START, nozzle_diameter=["0.4"])
        self.printer("Gone", machine_end_gcode="M84")
        self.commit()

    def diff(self):
        rc, out = self.run_command("gcode-diff", "--base", "HEAD")
        self.assertEqual(rc, 0, out)
        return out

    def test_identical_edits_share_one_section(self):
        for name in ("A", "B"):
            self.printer(name, machine_start_gcode="G28\nM104 S210  \nG1 Z5")
        self.printer("C", machine_start_gcode="G28\nM104 S220\nG1 Z5")
        out = self.diff()
        self.assertEqual(out.count("<details>"), 2)
        self.assertIn("<code>V/machine/A.json</code>, <code>V/machine/B.json</code> - "
                      "<code>machine_start_gcode</code>", out)
        self.assertIn("<code>V/machine/C.json</code> - <code>machine_start_gcode</code>", out)
        # Trailing spaces are removed.
        self.assertIn("\n-M104 S200\n+M104 S210\n", out)
        self.assertIn("\n\n```diff\n@@ ", out)
        self.assertIn("\n```\n\n</details>", out)

    def test_nothing_is_printed_without_a_text_change(self):
        self.printer("A", machine_start_gcode=self.START, nozzle_diameter=["0.6"])
        self.assertEqual(self.diff(), "")

    def test_added_and_deleted_files_show_the_whole_value(self):
        # An untracked file counts as added; the validator's user dir is not a profile.
        self.printer("New", machine_start_gcode="G28\nG29")
        self.t.write("user", "default/machine/U.json", {"machine_start_gcode": "G28"})
        os.remove(self.t.path("V", "machine/Gone.json"))
        out = self.diff()
        self.assertIn("@@ -0,0 +1,2 @@\n+G28\n+G29\n", out)
        self.assertIn("<code>V/machine/Gone.json</code> - <code>machine_end_gcode</code>", out)
        self.assertIn("@@ -1 +0,0 @@\n-M84\n", out)
        self.assertNotIn("U.json", out)

    def test_a_multi_line_value_counts_whatever_its_key(self):
        self.printer("A", machine_start_gcode=self.START, custom_note="one\ntwo")
        self.assertIn("<code>V/machine/A.json</code> - <code>custom_note</code>", self.diff())

    def test_a_file_name_cannot_mention_a_user(self):
        self.t.write("V", "filament/PLA @BBL A1.json", {"filament_start_gcode": ["M106"]})
        self.assertIn("<code>V/filament/PLA @BBL A1.json</code>", self.diff())

    def test_head_compares_two_commits(self):
        self.printer("A", machine_start_gcode="G28")
        self.commit()
        rc, out = self.run_command("gcode-diff", "--base", "HEAD~1", "--head", "HEAD")
        self.assertEqual(rc, 0, out)
        self.assertIn("<code>V/machine/A.json</code> - <code>machine_start_gcode</code>", out)

    def test_a_renamed_file_diffs_against_its_old_self(self):
        os.rename(self.t.path("V", "machine/A.json"), self.t.path("V", "machine/A2.json"))
        self.printer("A2", machine_start_gcode="G28\nM104 S210\nG1 Z5", nozzle_diameter=["0.4"])
        self.commit()
        rc, out = self.run_command("gcode-diff", "--base", "HEAD~1", "--head", "HEAD")
        self.assertEqual(rc, 0, out)
        self.assertIn("<code>V/machine/A2.json</code> - <code>machine_start_gcode</code>", out)
        self.assertIn("\n G28\n-M104 S200\n+M104 S210\n G1 Z5\n", out)

    def test_a_file_renamed_off_json_loses_its_values(self):
        os.rename(self.t.path("V", "machine/Gone.json"), self.t.path("V", "machine/Gone.json.bak"))
        self.commit()
        rc, out = self.run_command("gcode-diff", "--base", "HEAD~1", "--head", "HEAD")
        self.assertEqual(rc, 0, out)
        self.assertIn("<code>V/machine/Gone.json</code> - <code>machine_end_gcode</code>", out)
        self.assertIn("@@ -1 +0,0 @@\n-M84\n", out)

    def test_a_file_renamed_onto_json_is_new(self):
        with open(self.t.path("V", "machine/Draft.txt"), "w", encoding="utf-8") as f:
            json.dump({"type": "machine", "machine_start_gcode": self.START}, f)
        self.commit()
        os.rename(self.t.path("V", "machine/Draft.txt"), self.t.path("V", "machine/Draft.json"))
        self.commit()
        rc, out = self.run_command("gcode-diff", "--base", "HEAD~1", "--head", "HEAD")
        self.assertEqual(rc, 0, out)
        self.assertIn("@@ -0,0 +1,3 @@\n+G28\n+M104 S200\n+G1 Z5\n", out)

    def test_a_batch_read_survives_paths_that_are_not_files(self):
        blobs = apt._blobs_at(self.t.profiles, [("HEAD", "V/machine"), ("HEAD", "V/no such.json"),
                                                ("HEAD", "V/machine/Gone.json")])
        self.assertIsNone(blobs[("HEAD", "V/machine")])
        self.assertIsNone(blobs[("HEAD", "V/no such.json")])
        self.assertEqual(json.loads(blobs[("HEAD", "V/machine/Gone.json")])["machine_end_gcode"],
                         "M84")

    def test_each_value_of_a_vector_option_is_its_own_setting(self):
        self.t.write("V", "filament/F.json", {"filament_start_gcode": ["M106", "M107"]})
        self.commit()
        self.t.write("V", "filament/F.json", {"filament_start_gcode": ["M106", "M106 S0"]})
        out = self.diff()
        self.assertEqual(out.count("<details>"), 1)
        self.assertIn("<code>filament_start_gcode[1]</code>", out)

    def test_vendor_limits_the_files(self):
        self.printer("A", machine_start_gcode="G28")
        self.t.write("W", "machine/X.json", {"machine_start_gcode": "G28"})
        rc, out = self.run_command("gcode-diff", "--base", "HEAD", "--vendor", "V")
        self.assertEqual(rc, 0, out)
        self.assertIn("V/machine/A.json", out)
        self.assertNotIn("W/machine/X.json", out)

    def test_vendor_includes_files_moved_out_of_it(self):
        self.t.add_vendor("W")
        os.rename(self.t.path("V", "machine/A.json"), self.t.path("W", "machine/A.json"))
        data = self.t.read("W", "machine/A.json")
        self.t.write("W", "machine/A.json", {**data, "machine_start_gcode": "G28\nM104 S210\nG1 Z5"})
        self.commit()
        rc, out = self.run_command("gcode-diff", "--base", "HEAD~1", "--head", "HEAD",
                                   "--vendor", "V")
        self.assertEqual(rc, 0, out)
        self.assertIn("<code>W/machine/A.json</code> - <code>machine_start_gcode</code>", out)

    def same_edit(self, names):
        """The diff after one start G-code edit to each of names; returns (summary, output)."""
        for name in names:
            self.printer(name, machine_start_gcode=self.START, nozzle_diameter=["0.4"])
        self.commit()
        for name in names:
            self.printer(name, machine_start_gcode="G28")
        out = self.diff()
        return re.search(r"<summary>(.*)</summary>", out).group(1), out

    def test_a_long_file_list_moves_into_the_section(self):
        summary, out = self.same_edit("ABCDE")
        self.assertIn("<code>V/machine/C.json</code> and 2 more - ", summary)
        self.assertNotIn("D.json", summary)
        self.assertIn("<code>V/machine/D.json</code>, <code>V/machine/E.json</code></p>\n\n```diff",
                      out)

    def test_four_files_are_all_named_in_the_summary(self):
        summary, _out = self.same_edit("ABCD")
        self.assertIn("<code>V/machine/D.json</code> - ", summary)
        self.assertNotIn("more", summary)

    def test_an_error_goes_to_stderr_only(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc, out = self.run_command("gcode-diff", "--base", "no-such-ref")
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIn("no-such-ref", err.getvalue())

    def test_a_file_name_that_is_not_utf8_is_an_error(self):
        bad = UnicodeDecodeError("utf-8", b"\xe9.json", 0, 1, "invalid continuation byte")
        err = io.StringIO()
        with mock.patch.object(apt, "_changed_profiles", side_effect=bad), \
                contextlib.redirect_stderr(err):
            rc, out = self.run_command("gcode-diff", "--base", "HEAD")
        self.assertEqual((rc, out), (1, ""))
        self.assertIn("invalid continuation byte", err.getvalue())

    def test_output_is_cut_to_whole_sections(self):
        for name in ("A", "B", "C"):
            self.printer(name, machine_start_gcode=name * 300)
        size = len(apt.gcode_diff(self.t.profiles, "HEAD").encode("utf-8"))
        with mock.patch.object(apt, "GCODE_DIFF_BUDGET", size - 1):
            out = self.diff()
        self.assertEqual(out.count("<details>"), 2)
        self.assertEqual(out.count("</details>"), 2)
        self.assertIn("\n1 of 3 sections is not shown", out)
        with mock.patch.object(apt, "GCODE_DIFF_BUDGET", 1):
            self.assertIn("\n3 of 3 sections are not shown", self.diff())

    def test_cleanup_removes_the_repo(self):
        # setUp's commit leaves read-only git objects, and a read-only directory blocks
        # deletion on POSIX.
        os.chmod(self.t.path("V", "machine"), stat.S_IRUSR | stat.S_IXUSR)
        self.t.cleanup()
        self.assertFalse(os.path.exists(self.t.dir))
        with mock.patch.object(os, "chmod") as chmod:
            self.t.cleanup()  # a second cleanup, as addCleanup runs, must not touch the temp dir
        chmod.assert_not_called()

    def test_only_repository_variables_are_stripped(self):
        self.assertTrue({"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"} <= self.repo_env_vars)
        self.assertFalse({"GIT_SSH", "GIT_EXEC_PATH", "GIT_EDITOR"} & self.repo_env_vars)

    def test_cleanup_leaves_symlink_targets_alone(self):
        outside = tempfile.mkdtemp(prefix="profile_tool_outside_")
        self.addCleanup(shutil.rmtree, outside, True)
        target = os.path.join(outside, "f")
        open(target, "w").close()
        os.chmod(target, stat.S_IRUSR)
        self.addCleanup(os.chmod, target, stat.S_IRWXU)  # runs before the rmtree above
        machine = self.t.path("V", "machine")
        try:
            os.symlink(target, os.path.join(machine, "link"))
        except (OSError, NotImplementedError):
            self.skipTest("cannot create symlinks here")
        os.chmod(machine, stat.S_IRUSR | stat.S_IXUSR)  # so removing the link fails at first
        self.t.cleanup()
        self.assertFalse(os.path.exists(self.t.dir))
        self.assertEqual(stat.S_IMODE(os.stat(target).st_mode) & stat.S_IWUSR, 0)

    def test_a_git_hook_environment_is_ignored(self):
        scripts = os.path.join(REPO_ROOT, "scripts")
        result = subprocess.run(
            [sys.executable, "-m", "unittest",
             "tests.test_profile_tool.TestGcodeDiff.test_head_compares_two_commits"],
            cwd=scripts, capture_output=True, text=True,
            env={**os.environ, "GIT_DIR": os.path.join(self.t.dir, "nowhere")})
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_the_options_are_read_from_the_engine(self):
        keys = apt._text_option_keys()
        self.assertTrue({"machine_start_gcode", "filament_start_gcode", "printing_by_object_gcode",
                         "notes", "post_process"} <= keys)
        self.assertNotIn("emit_machine_limits_to_gcode", keys)  # a coBool

    START_DEF = 'def = this->add("machine_start_gcode", coString);\n'

    def keys_from(self, print_config, tab=None):
        """_text_option_keys read from these sources; no tab means no Tab.cpp."""
        paths = [os.path.join(self.t.dir, "PrintConfig.cpp"), os.path.join(self.t.dir, "Tab.cpp")]
        for path, text in zip(paths, (print_config, tab)):
            if text is not None:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)
        apt._text_option_keys.cache_clear()
        self.addCleanup(apt._text_option_keys.cache_clear)
        with mock.patch.object(apt, "PRINT_CONFIG_CPP", paths[0]), \
                mock.patch.object(apt, "TAB_CPP", paths[1]):
            return apt._text_option_keys()

    def test_multiline_is_read_whatever_its_spacing(self):
        keys = self.keys_from(self.START_DEF + 'def = this->add("printer_notes", coString);\n'
                                               '    def->multiline  = true;\n')
        self.assertIn("printer_notes", keys)

    def test_the_code_options_of_tab_cpp_are_read(self):
        keys = self.keys_from(self.START_DEF, 'option = optgroup->get_option("filename_format");\n'
                                              'option.opt.is_code = true;\n'
                                              'option = optgroup->get_option("layer_height");\n')
        self.assertIn("filename_format", keys)
        self.assertNotIn("layer_height", keys)

    def test_a_missing_tab_cpp_is_not_an_error(self):
        self.assertIn("machine_start_gcode", self.keys_from(self.START_DEF))

    def test_markdown_in_a_file_name_stays_literal(self):
        section = apt._gcode_diff_section(["V/a *b*.json", "V/c.json", "V/d.json", "V/e.json",
                                           "V/f.json"], "machine_start_gcode", "@@ -1 +1 @@")
        self.assertIn("\n<p><code>V/a *b*.json</code>, ", section)


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
