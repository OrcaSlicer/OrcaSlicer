#!/usr/bin/env python3
"""Compare two synthetic infill fixtures using an existing remote CLI binary."""

import argparse
import datetime as dt
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys

from run_matrix import INFILL_ROLES, compare_cases, create_fixtures, digest, run_case, write_json


FOOTER_KEYS = (
    "reduce_infill_retraction", "sparse_infill_density", "sparse_infill_pattern",
    "infill_anchor", "infill_anchor_max", "top_shell_layers", "bottom_shell_layers",
    "top_shell_thickness", "bottom_shell_thickness", "wall_loops",
    "minimum_sparse_infill_area", "infill_combination", "infill_combination_max_layer_height",
    "layer_height", "infill_direction", "infill_wall_overlap",
)
CONFIG_VALUE = re.compile(r"^\s*;\s*([a-z_]+)\s*=\s*(.*?)\s*$")
ABSOLUTE_NUMBER = re.compile(r"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$")


def footer_fields(path):
    """Extract selected settings from actual generated configuration blocks."""
    values = {key: [] for key in FOOTER_KEYS}
    inside_config = False
    with path.open(encoding="utf-8", errors="replace") as gcode:
        for line in gcode:
            marker = line.strip().lstrip(";").strip()
            if marker == "CONFIG_BLOCK_START":
                inside_config = True
                continue
            if marker == "CONFIG_BLOCK_END":
                inside_config = False
                continue
            match = CONFIG_VALUE.match(line) if inside_config else None
            if match and match[1] in values:
                value = " ".join(match[2].split())
                if value not in values[match[1]]:
                    values[match[1]].append(value)
    return values


def verify_effective_anchors(fields, expected_maximum):
    """Verify absolute millimeter values, including every repeated footer value.

    Percent encodings, including zero percent, are rejected so the experiment
    cannot silently compare different units. Equivalent finite numeric
    spellings are accepted with a 1e-9 tolerance.
    """
    expected = {"infill_anchor": 0.0, "infill_anchor_max": expected_maximum}
    observed = {key: fields.get(key, []) for key in expected}
    errors = []
    for key, wanted in expected.items():
        if not observed[key]:
            errors.append(key + ": missing from generated G-code configuration")
        for raw in observed[key]:
            if not ABSOLUTE_NUMBER.fullmatch(raw):
                errors.append(key + ": expected an absolute numeric millimeter value, observed " + repr(raw))
                continue
            value = float(raw)
            if not math.isfinite(value) or not math.isclose(value, wanted, rel_tol=1e-9, abs_tol=1e-9):
                errors.append(key + ": expected " + str(wanted) + " mm, observed " + repr(raw))
    return {"expected_mm": expected, "observed_footer_values": observed,
            "verified": not errors, "errors": errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    if sys.platform not in {"linux", "darwin"}:
        parser.error("this probe uses the Linux/macOS CLI harness")
    if not 1 <= args.timeout <= 1800:
        parser.error("--timeout must be between 1 and 1800 seconds per slice")
    args.binary = args.binary.resolve(strict=True)
    args.source = args.source.resolve(strict=True)
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error("--binary must name an existing executable")
    root = args.output.resolve()
    if root.exists():
        parser.error("--output must name a new directory")
    root.mkdir(parents=True)

    fixtures_by_variant, settings = {}, {}
    for name, anchor_max in (("control-anchor-0.1", "0.1"), ("treatment-anchor-0", "0")):
        pair_root = root / name
        fixtures = pair_root / "fixtures"
        create_fixtures(fixtures)
        process_path = fixtures / "process.json"
        process = json.loads(process_path.read_text(encoding="utf-8"))
        process["infill_anchor_max"] = anchor_max
        write_json(process_path, process)
        fixtures_by_variant[name] = fixtures
        settings[name] = process
    control_settings, treatment_settings = settings.values()
    differences = {key: {"control": control_settings.get(key), "treatment": treatment_settings.get(key)}
                   for key in control_settings.keys() | treatment_settings.keys()
                   if control_settings.get(key) != treatment_settings.get(key)}
    if differences != {"infill_anchor_max": {"control": "0.1", "treatment": "0"}}:
        raise RuntimeError("probe variants must differ only in infill_anchor_max")

    pairs = {}
    for name, fixtures in fixtures_by_variant.items():
        pair_root = fixtures.parent
        expected_maximum = float(settings[name]["infill_anchor_max"])
        off = run_case(args, pair_root, fixtures, "off", False)
        on = run_case(args, pair_root, fixtures, "on", True)
        for case in (off, on):
            for gcode in case["gcode"]:
                gcode["normalized_footer_fields"] = footer_fields(pair_root / case["name"] / gcode["path"])
                gcode["effective_anchor_validation"] = verify_effective_anchors(
                    gcode["normalized_footer_fields"], expected_maximum)
            case["effective_anchor_verified"] = bool(case["gcode"]) and all(
                gcode["effective_anchor_validation"]["verified"] for gcode in case["gcode"])
            write_json(pair_root / case["name"] / "result.json", case)
        anchors_verified = off["effective_anchor_verified"] and on["effective_anchor_verified"]
        comparison = compare_cases(pair_root, off, on, INFILL_ROLES, "infill")
        observed = (comparison["retraction_added_with_preference_on"] > 0
                    and comparison["returned_z_hop_added_with_preference_on"] > 0)
        coverage = "motion difference observed" if observed else "coverage gap"
        if name == "control-anchor-0.1" and comparison["matched_travels"] == 0:
            coverage = "expected control coverage gap; no qualifying infill travels matched"
        if not anchors_verified:
            coverage = "effective anchors unverified; observed motion cannot be attributed to requested anchor values"
        pairs[name] = {
            "requested_input_anchors_mm": {"infill_anchor": 0.0, "infill_anchor_max": expected_maximum},
            "fixture_sha256": {path.name: digest(path.read_bytes()) for path in sorted(fixtures.iterdir())},
            "cases": [off, on], "slices_and_configuration_valid": off["passed"] and on["passed"],
            "effective_anchor_verified": anchors_verified,
            "infill_motion_comparison": comparison, "infill_motion_observed": observed,
            "coverage_classification": coverage,
        }
        write_json(pair_root / "pair-probe.json", pairs[name])

    source_head = subprocess.run(["/usr/bin/git", "rev-parse", "HEAD"], cwd=args.source,
                                 capture_output=True, text=True, check=True, timeout=30).stdout.strip()
    valid = all(pair["slices_and_configuration_valid"] for pair in pairs.values())
    anchors_verified = all(pair["effective_anchor_verified"] for pair in pairs.values())
    probe_valid = valid and anchors_verified
    report = {
        "started_from_existing_binary": True, "reported_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source_checkout_head": source_head, "binary": str(args.binary),
        "binary_sha256": digest(args.binary.read_bytes()), "platform": sys.platform,
        "input_process_differences": differences, "pairs": pairs,
        "slices_and_configuration_valid": valid,
        "effective_anchor_verified": anchors_verified, "probe_valid": probe_valid,
        "treatment_infill_motion_observed": pairs["treatment-anchor-0"]["infill_motion_observed"],
        "exit_semantics": "Zero requires four successful slices with expected reduce options and verified absolute anchors in every G-code file; motion coverage is separate.",
        "source_basis": "FillParams::dont_connect() in src/libslic3r/Fill/FillBase.hpp uses anchor_length_max < 0.05f.",
        "limitations": [
            "This diagnostic does not change or weaken the canonical application matrix gate.",
            "The control can validly report zero matches; treatment coverage must be read from its explicit boolean.",
            "Matched role/XYZ/E/Z observations do not establish exact internal-region branch execution or print quality.",
            "Source checkout and executable hashes are separate; retained checkpoint/build provenance establishes their relationship.",
        ],
    }
    write_json(root / "infill-probe.json", report)
    print("BOUNTY_17866_PROBE=" + json.dumps(report, separators=(",", ":")), flush=True)
    return 0 if probe_valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
