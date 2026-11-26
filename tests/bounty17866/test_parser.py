"""Harness self-checks using synthetic G-code; these never run OrcaSlicer."""

import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import run_matrix
from run_matrix import summarize_gcode


def travel_text(role="Support", retract=True, hop=True):
    lines = ["M83", "G90", "G1 X0 Y0 Z0.2", ";TYPE:" + role, "G1 X10 E0.5"]
    if retract:
        lines.append("G1 E-0.8")
    if hop:
        lines.append("G1 Z0.6")
    lines.append("G1 X14")
    if hop:
        lines.append("G1 Z0.2")
    if retract:
        lines.append("G1 E0.8")
    lines.append("G1 X20 E0.5")
    return "\n".join(lines) + "\n"


class ParserChecks(unittest.TestCase):
    def test_relative_withdrawal(self):
        result = summarize_gcode("M83\nG1 E-0.8\nG1 E0.8\nG1 X5 E0.2\n")
        self.assertEqual(result["retraction_moves"], 1)
        self.assertEqual(result["withdrawn_filament_mm"], 0.8)

    def test_absolute_extrusion_and_reset(self):
        result = summarize_gcode("M82\nG92 E10\nG1 E9.2\nG92 E0\nG1 E0.8\n")
        self.assertEqual(result["retraction_moves"], 1)
        self.assertEqual(result["withdrawn_filament_mm"], 0.8)

    def test_positive_z_moves_are_diagnostic(self):
        result = summarize_gcode("G90\nG1 Z0.2\nG1 Z0.6\nG1 Z0.2\nG91\nG1 Z0.4\nG1 Z-0.4\n")
        self.assertEqual(result["positive_z_moves"], 3)
        self.assertIn("not proof of Z-hop", result["motion_limit"])

    def test_header_ignores_inline_toolpath_comments(self):
        result = summarize_gcode("; reduce_infill_retraction = 0\nG1 E1 ; reduce_infill_retraction = 1\n")
        self.assertEqual(result["reduce_infill_retraction_values"], [0])

    def test_comments_do_not_change_motion_hash(self):
        a = summarize_gcode("; yesterday\nG1 X1 E0.1 ; first\n")
        b = summarize_gcode("; today\nG1 X1 E0.1 ; second\n")
        self.assertEqual(a["toolpath_commands_sha256"], b["toolpath_commands_sha256"])

    def test_support_travel_contains_actual_e_and_z_sequence(self):
        result = summarize_gcode(travel_text())
        sequences = result.get("role_travel_sequences", [])
        self.assertEqual(len(sequences), 1)
        self.assertEqual(sequences[0]["role"], "Support")
        self.assertEqual(sequences[0]["xy_distance_mm"], 4)
        self.assertEqual(sequences[0]["withdrawn_filament_mm"], 0.8)
        self.assertEqual(sequences[0]["returned_z_hop_mm"], 0.4)
        self.assertIn("G1 E-0.8", [step["command"] for step in sequences[0]["moves"]])

    def test_same_layer_travel_without_retraction_is_recorded(self):
        sequences = summarize_gcode(travel_text(retract=False, hop=False)).get("role_travel_sequences", [])
        self.assertEqual(len(sequences), 1)
        self.assertEqual(sequences[0]["withdrawn_filament_mm"], 0)
        self.assertEqual(sequences[0]["returned_z_hop_mm"], 0)

    def test_layer_change_is_not_a_returned_hop_sequence(self):
        text = travel_text().replace("G1 Z0.2\nG1 E0.8", "G1 Z0.4\nG1 E0.8")
        self.assertEqual(summarize_gcode(text).get("role_travel_sequences"), [])

    def test_different_role_extrusions_do_not_make_support_internal_travel(self):
        text = travel_text().replace("G1 X20 E0.5", ";TYPE:Outer wall\nG1 X20 E0.5")
        self.assertEqual(summarize_gcode(text).get("role_travel_sequences"), [])

    def test_absolute_e_and_relative_xyz_preserve_sequence_geometry(self):
        text = ("M82\nG90\nG1 X10 Y0 Z0.2\n;TYPE:Sparse infill\nG1 X20 E10\n"
                "G1 E9.2\nG91\nG1 Z0.4\nG1 X4\nG1 Z-0.4\nG92 E0\nG1 E0.8\nG1 X6 E1.3\n")
        sequences = summarize_gcode(text).get("role_travel_sequences", [])
        self.assertEqual(len(sequences), 1)
        self.assertEqual(sequences[0]["start_xyz"], [20, 0, 0.2])
        self.assertEqual(sequences[0]["end_xyz"], [24, 0, 0.2])
        self.assertEqual(sequences[0]["withdrawn_filament_mm"], 0.8)

    def test_matching_compares_same_geometry_and_keeps_real_sequences(self):
        compare = getattr(run_matrix, "compare_travels", None)
        self.assertTrue(callable(compare), "travel comparison must be implemented")
        off = summarize_gcode(travel_text(retract=False, hop=False))["role_travel_sequences"]
        on = summarize_gcode(travel_text())["role_travel_sequences"]
        comparison = compare(off, on, {"Support"})
        self.assertEqual(comparison["matched_travels"], 1)
        self.assertEqual(comparison["retraction_added_with_preference_on"], 1)
        self.assertEqual(comparison["returned_z_hop_added_with_preference_on"], 1)

    def test_matching_does_not_reuse_one_off_sequence_for_repeated_geometry(self):
        off = summarize_gcode(travel_text(retract=False, hop=False))["role_travel_sequences"]
        on = summarize_gcode(travel_text())["role_travel_sequences"]
        comparison = run_matrix.compare_travels(off, on + on, {"Support"})
        self.assertEqual(comparison["matched_travels"], 1)
        self.assertEqual(comparison["unmatched_on"], 1)

    def test_short_moves_do_not_count_as_qualifying_travel(self):
        text = travel_text().replace("G1 X14", "G1 X10.5")
        self.assertEqual(summarize_gcode(text)["role_travel_sequences"], [])

    def test_preference_is_written_only_to_the_app_category(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_matrix.set_preference(root, True)
            self.assertEqual(json.loads((root / "OrcaSlicer.conf").read_text()),
                             {"app": {run_matrix.KEY: True}})
            run_matrix.set_preference(root, None)
            self.assertEqual(json.loads((root / "OrcaSlicer.conf").read_text()), {"app": {}})

    def test_default_directory_matches_each_platform(self):
        lookup = getattr(run_matrix, "default_config_directory", None)
        self.assertTrue(callable(lookup), "platform lookup must be implemented")
        fixture_home = Path("/tmp/fixture-home")
        self.assertEqual(lookup(fixture_home, "linux"), fixture_home / ".config/OrcaSlicer")
        self.assertEqual(lookup(fixture_home, "darwin"), fixture_home / "Library/Application Support/OrcaSlicer")

    def test_display_and_build_library_environment_is_allowlisted(self):
        sanitize = getattr(run_matrix, "sanitized_env", None)
        self.assertTrue(callable(sanitize), "isolated environment must be implemented")
        with patch.dict(os.environ, {"DISPLAY": ":99", "XAUTHORITY": "/tmp/xauth", "LD_LIBRARY_PATH": "/tmp/deps/lib", "EXAMPLE_SECRET": "never copy"}):
            env = sanitize(Path("/tmp/fixture-home"), Path("/tmp/tmp"))
        self.assertEqual(env["DISPLAY"], ":99")
        self.assertEqual(env["LD_LIBRARY_PATH"], "/tmp/deps/lib")
        self.assertEqual(env["XAUTHORITY"], "/tmp/xauth")
        self.assertNotIn("EXAMPLE_SECRET", env)
        self.assertEqual(env.get("HOME"), os.environ.get("HOME"))

    def test_synthetic_presets_include_infill_and_support_models(self):
        create = getattr(run_matrix, "create_fixtures", None)
        self.assertTrue(callable(create), "fixture generation must be implemented")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            create(root)
            process = json.loads((root / "process.json").read_text())
            self.assertEqual(process["reduce_infill_retraction"], "1")
            self.assertEqual((root / "cube.stl").read_text().count("facet normal"), 12)
            for kind in ("normal", "tree"):
                support = json.loads((root / (kind + ".json")).read_text())
                self.assertEqual(support["enable_support"], "1")
                self.assertEqual(support["support_type"], kind + "(auto)")
            self.assertTrue((root / "capital.stl").is_file())


if __name__ == "__main__":
    unittest.main()
