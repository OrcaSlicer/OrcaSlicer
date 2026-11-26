"""Probe evidence checks; controlled CLI output never invokes OrcaSlicer."""

from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import probe_infill
import run_matrix


class ProbeEvidenceChecks(unittest.TestCase):
    def run_probe(self, root, violation=None):
        """Keep real run_case/parsing/files; replace only external CLI execution."""
        binary = root / "unexecuted-test-binary"
        binary.write_text("This executable fixture is never launched.\n")
        binary.chmod(0o700)
        output = root / "probe"

        def controlled_cli(command, cwd, env, timeout, log_path):
            settings_paths = command[command.index("--load-settings") + 1].split(";")
            process = json.loads(Path(settings_paths[1]).read_text())
            datadir = Path(command[command.index("--datadir") + 1])
            preference = json.loads((datadir / "OrcaSlicer.conf").read_text())["app"][run_matrix.KEY]
            effective_option = 0 if preference else 1
            anchor = "0.0"
            maximum = "0.1000" if process["infill_anchor_max"] == "0.1" else "0.0"
            # One treatment output violates the contract; its reduce option stays valid.
            selected = process["infill_anchor_max"] == "0" and preference
            if selected and violation == "wrong-maximum":
                maximum = "0.1"
            if selected and violation == "missing-anchor":
                anchor = None
            footer = ["; CONFIG_BLOCK_START", "; reduce_infill_retraction = " + str(effective_option)]
            if anchor is not None:
                footer.append("; infill_anchor = " + anchor)
            footer.extend(["; infill_anchor_max = " + maximum, "; CONFIG_BLOCK_END"])
            moves = ["G90", "M83", "G1 X0 Y0 Z0.2", ";TYPE:Sparse infill", "G1 X10 E0.5"]
            if preference:
                moves.extend(["G1 E-0.8", "G1 Z0.6", "G1 X14", "G1 Z0.2", "G1 E0.8"])
            else:
                moves.append("G1 X14")
            moves.append("G1 X20 E0.5")
            (cwd / "fixture.gcode").write_text("\n".join(moves + footer) + "\n")
            log_path.write_text("Controlled CLI boundary; no application launched.\n")
            run_matrix.write_json(cwd / "result.json", {"return_code": 0, "warnings": []})
            with zipfile.ZipFile(cwd / "project.3mf", "w") as archive:
                archive.writestr("Metadata/project_settings.config", json.dumps(process))
            return 0, None

        arguments = ["probe_infill.py", "--binary", str(binary), "--source",
                     str(Path(__file__).resolve().parents[2]), "--output", str(output)]
        with patch("sys.argv", arguments), patch.object(run_matrix, "execute", controlled_cli), redirect_stdout(io.StringIO()):
            status = probe_infill.main()
        return status, json.loads((output / "infill-probe.json").read_text())

    def test_probe_rejects_wrong_or_missing_effective_anchors_despite_valid_reduce_options(self):
        for violation in ("wrong-maximum", "missing-anchor"):
            with self.subTest(violation=violation), tempfile.TemporaryDirectory() as temporary:
                status, report = self.run_probe(Path(temporary), violation)
                self.assertNotEqual(status, 0, "ineffective fixture anchors must fail the probe")
                self.assertTrue(report["slices_and_configuration_valid"])
                self.assertFalse(report["effective_anchor_verified"])
                treatment = report["pairs"]["treatment-anchor-0"]
                self.assertFalse(treatment["effective_anchor_verified"])
                self.assertFalse(treatment["cases"][1]["effective_anchor_verified"])
                # These are real parser/comparison observations, separate from fixture validity.
                self.assertTrue(report["treatment_infill_motion_observed"])

    def test_probe_accepts_equivalent_numeric_anchor_notation_in_generated_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            status, report = self.run_probe(Path(temporary))
            self.assertEqual(status, 0)
            self.assertTrue(report.get("effective_anchor_verified"), "successful probes must verify observed anchors")
            for pair in report["pairs"].values():
                self.assertTrue(pair["effective_anchor_verified"])
                for case in pair["cases"]:
                    self.assertTrue(case["effective_anchor_verified"])

    def test_anchor_validation_accepts_numeric_equivalence(self):
        validate = getattr(probe_infill, "verify_effective_anchors", None)
        self.assertTrue(callable(validate), "effective anchor validation is required")
        result = validate({"infill_anchor": ["0", "0.0"], "infill_anchor_max": ["0.1000", "1e-1"]}, 0.1)
        self.assertTrue(result["verified"])

    def test_anchor_validation_rejects_missing_conflicting_nonfinite_and_percent_values(self):
        validate = getattr(probe_infill, "verify_effective_anchors", None)
        self.assertTrue(callable(validate), "effective anchor validation is required")
        for values in ([], ["0.1", "0"], ["NaN"], ["inf"], ["1e999"], ["0.1%"], ["0%"]):
            with self.subTest(values=values):
                result = validate({"infill_anchor": ["0"], "infill_anchor_max": values}, 0.1)
                self.assertFalse(result["verified"])
                self.assertTrue(result["errors"])

    def test_anchor_validation_rejects_nonzero_anchor_even_with_correct_maximum(self):
        validate = getattr(probe_infill, "verify_effective_anchors", None)
        self.assertTrue(callable(validate), "effective anchor validation is required")
        result = validate({"infill_anchor": ["0.2"], "infill_anchor_max": ["0.1"]}, 0.1)
        self.assertFalse(result["verified"])


if __name__ == "__main__":
    unittest.main()
