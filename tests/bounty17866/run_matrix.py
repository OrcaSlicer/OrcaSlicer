#!/usr/bin/env python3
"""Slice synthetic fixtures with an existing OrcaSlicer CLI; never fetch assets."""

import argparse
from collections import Counter, defaultdict, deque
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import zipfile


KEY = "global_disable_interference_travel"
HEADER = re.compile(r"^\s*;\s*reduce_infill_retraction\s*=\s*([01])\s*$", re.M)
WORDS = re.compile(r"([A-Z])\s*([-+]?(?:\d+(?:\.\d*)?|\.\d+))", re.I)
TOOLPATH = {"G0", "G1", "G2", "G3", "G10", "G11", "G90", "G91", "G92", "M82", "M83"}
SUPPORT_ROLES = {"Support", "Support transition", "Support interface"}
INFILL_ROLES = {"Sparse infill", "Internal solid infill"}
EPSILON = 1e-7


def digest(data):
    return hashlib.sha256(data).hexdigest()


def summarize_gcode(text):
    """Keep actual moves between same-role, same-Z extrusion segments.

    A candidate has >= 1mm of non-extruding XY travel. A returned Z excursion
    is measured only between those extrusion segments, excluding layer changes.
    G-code roles do not establish containment in an internal/support island.
    """
    e_relative, xyz_relative = False, False
    position = dict.fromkeys("XYZE", 0.0)
    retracts = z_up = 0
    withdrawal = 0.0
    commands, sequences, pending = [], [], []
    role, previous = None, None
    role_extrusions = Counter()
    for line_number, raw in enumerate(text.splitlines(), 1):
        if raw.strip().startswith(";TYPE:"):
            role = raw.strip()[6:].strip()
        command = raw.partition(";")[0].strip().upper()
        if not command:
            continue
        words = WORDS.findall(command)
        if not words:
            continue
        letter, number = words[0]
        opcode = letter + str(int(float(number)))
        parameters = {key: float(value) for key, value in words[1:]}
        if opcode in TOOLPATH:
            commands.append(" ".join(command.split()))
        if opcode == "M82":
            e_relative = False
        elif opcode == "M83":
            e_relative = True
        elif opcode in {"G90", "G91"}:
            xyz_relative = opcode == "G91"
        elif opcode == "G92":
            position.update({k: v for k, v in parameters.items() if k in position})
        elif opcode in {"G0", "G1", "G2", "G3"}:
            before = position.copy()
            for axis, value in parameters.items():
                if axis in position:
                    relative = e_relative if axis == "E" else xyz_relative
                    position[axis] = position[axis] + value if relative else value
            delta_e = position["E"] - before["E"]
            delta_z = position["Z"] - before["Z"]
            xy_distance = math.hypot(position["X"] - before["X"], position["Y"] - before["Y"])
            retracts += delta_e < -EPSILON
            withdrawal += max(0, -delta_e)
            z_up += delta_z > EPSILON
            depositing = delta_e > EPSILON and (xy_distance > EPSILON or opcode in {"G2", "G3"})
            if depositing:
                role_extrusions[role or "unlabelled"] += 1
                if previous and previous["role"] == role and role in SUPPORT_ROLES | INFILL_ROLES:
                    distance = sum(step["xy_distance_mm"] for step in pending)
                    if distance >= 1 - EPSILON and abs(previous["xyz"][2] - before["Z"]) < EPSILON:
                        start = previous["xyz"]
                        peak = max([start[2]] + [step["end_xyz"][2] for step in pending])
                        sequences.append({
                            "role": role, "from_extrusion_line": previous["line"],
                            "to_extrusion_line": line_number,
                            "start_xyz": start, "end_xyz": [round(before[a], 6) for a in "XYZ"],
                            "xy_distance_mm": round(distance, 6),
                            "withdrawn_filament_mm": round(sum(max(0, -s["delta_e_mm"]) for s in pending), 6),
                            "returned_z_hop_mm": round(max(0, peak - start[2]), 6),
                            "moves": pending,
                        })
                previous = {"role": role, "xyz": [round(position[a], 6) for a in "XYZ"], "line": line_number}
                pending = []
            elif previous:
                pending.append({"line": line_number, "command": command,
                                "delta_e_mm": round(delta_e, 6), "delta_z_mm": round(delta_z, 6),
                                "xy_distance_mm": round(xy_distance, 6),
                                "end_xyz": [round(position[a], 6) for a in "XYZ"]})
    return {
        "reduce_infill_retraction_values": [int(v) for v in HEADER.findall(text)],
        "retraction_moves": retracts,
        "withdrawn_filament_mm": round(withdrawal, 6),
        "positive_z_moves": z_up,
        "toolpath_command_count": len(commands),
        "toolpath_commands_sha256": digest("\n".join(commands).encode()),
        "motion_limit": "Positive Z moves include layer changes and are not proof of Z-hop.",
        "role_extrusion_moves": dict(role_extrusions),
        "role_travel_counts": dict(Counter(s["role"] for s in sequences)),
        "role_travel_sequences": sequences,
    }


def compare_travels(off_sequences, on_sequences, roles):
    """Match endpoints and layer Z, preserving repeated occurrences separately."""
    def key(sequence):
        return (sequence["role"], tuple(sequence["start_xyz"]), tuple(sequence["end_xyz"]))

    off_by_geometry = defaultdict(deque)
    for sequence in off_sequences:
        if sequence["role"] in roles:
            off_by_geometry[key(sequence)].append(sequence)
    matches, unmatched_on = [], 0
    for on in on_sequences:
        if on["role"] not in roles:
            continue
        candidates = off_by_geometry[key(on)]
        if not candidates:
            unmatched_on += 1
            continue
        off = candidates.popleft()
        matches.append({"role": on["role"], "start_xyz": on["start_xyz"], "end_xyz": on["end_xyz"],
                        "off": off, "on": on})
    return {
        "matched_travels": len(matches), "unmatched_on": unmatched_on,
        "unmatched_off": sum(len(v) for v in off_by_geometry.values()),
        "retraction_added_with_preference_on": sum(
            pair["on"]["withdrawn_filament_mm"] > EPSILON and pair["off"]["withdrawn_filament_mm"] <= EPSILON
            for pair in matches),
        "returned_z_hop_added_with_preference_on": sum(
            pair["on"]["returned_z_hop_mm"] > EPSILON and pair["off"]["returned_z_hop_mm"] <= EPSILON
            for pair in matches),
        "no_retraction_in_either_state": sum(
            pair["on"]["withdrawn_filament_mm"] <= EPSILON and pair["off"]["withdrawn_filament_mm"] <= EPSILON
            for pair in matches),
        "matched_sequences": matches,
        "interpretation": "Same-role extrusion brackets and matching endpoints are observable; internal/support-island containment is not established by G-code.",
    }


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def box_triangles(x0, y0, z0, x1, y1, z1):
    vertices = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        yield [vertices[i] for i in (a, b, c)]
        yield [vertices[i] for i in (a, c, d)]


def write_stl(path, triangles):
    lines = ["solid synthetic_fixture"]
    for triangle in triangles:
        lines.extend(["facet normal 0 0 0", "outer loop"])
        lines.extend("vertex %g %g %g" % vertex for vertex in triangle)
        lines.extend(["endloop", "endfacet"])
    lines.append("endsolid synthetic_fixture")
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def create_fixtures(root):
    root.mkdir(parents=True, exist_ok=True)
    # Standalone presets and overlapping-box capital follow tests/cli/test_cli_strict.sh.
    write_json(root / "machine.json", {
        "type": "machine", "from": "User", "name": "Synthetic interference fixture printer",
        "printable_area": ["0x0", "200x0", "200x200", "0x200"],
        "printable_height": "100", "gcode_flavor": "marlin2", "nozzle_diameter": ["0.4"],
        "machine_start_gcode": "G90\nM83", "machine_end_gcode": "", "layer_change_gcode": "",
        "use_relative_e_distances": "1", "retraction_length": ["0.8"],
        "retraction_minimum_travel": ["1"], "retract_when_changing_layer": ["0"],
        "wipe": ["0"], "z_hop": ["0.4"], "z_hop_types": ["Normal Lift"],
    })
    process = {
        "type": "process", "from": "User", "name": "Synthetic interference fixture process",
        "enable_support": "0", "reduce_infill_retraction": "1",
        "layer_height": "0.2", "initial_layer_print_height": "0.2",
        "sparse_infill_density": "15%", "sparse_infill_pattern": "rectilinear",
        # Limit infill anchoring so this convex model yields separate infill roads.
        "infill_anchor": "0", "infill_anchor_max": "0.1",
        "wall_loops": "2", "top_shell_layers": "0", "bottom_shell_layers": "0",
        "gcode_comments": "1", "filename_format": "fixture.gcode",
    }
    write_json(root / "process.json", process)
    for kind in ("normal", "tree"):
        support = dict(process, name="Synthetic " + kind + " support fixture", enable_support="1",
                       support_type=kind + "(auto)", support_threshold_angle="45",
                       support_on_build_plate_only="1", support_base_pattern_spacing="2.5",
                       support_interface_top_layers="0", support_interface_bottom_layers="0",
                       independent_support_layer_height="0", top_shell_layers="2", bottom_shell_layers="2")
        support.update({"support_style": "grid" if kind == "normal" else "tree_slim",
                        "support_base_pattern": "rectilinear" if kind == "normal" else "lightning"})
        write_json(root / (kind + ".json"), support)
    write_json(root / "filament.json", {
        "type": "filament", "from": "User", "name": "Synthetic fixture PLA",
        "filament_type": ["PLA"], "nozzle_temperature": ["210"],
        "nozzle_temperature_initial_layer": ["210"], "filament_max_volumetric_speed": ["12"],
        "filament_diameter": ["1.75"], "filament_density": ["1.24"], "filament_flow_ratio": ["1"],
    })
    write_stl(root / "cube.stl", box_triangles(20, 20, 0, 60, 60, 6))
    # 40 x 40 x 14mm capital: an 8mm stem meets a 2mm cap, with 1mm overlap.
    write_stl(root / "capital.stl", [*box_triangles(16, 16, 0, 24, 24, 13),
                                     *box_triangles(0, 0, 12, 40, 40, 14)])


def set_preference(directory, state):
    directory.mkdir(parents=True, exist_ok=True)
    write_json(directory / "OrcaSlicer.conf", {"app": {} if state is None else {KEY: state}})


def default_config_directory(fixture_home, platform=None):
    if (platform or sys.platform) == "darwin":
        return fixture_home / "Library/Application Support/OrcaSlicer"
    return fixture_home / ".config/OrcaSlicer"


def sanitized_env(fixture_home, temporary):
    # HOME is preserved. Only the child process gets isolated config/cache roots.
    env = {"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C.UTF-8"}
    for name in ("HOME", "DISPLAY", "XAUTHORITY", "LD_LIBRARY_PATH", "LIBGL_ALWAYS_SOFTWARE", "GALLIUM_DRIVER"):
        if name in os.environ:
            env[name] = os.environ[name]
    env.update({"CFFIXED_USER_HOME": str(fixture_home), "XDG_CONFIG_HOME": str(fixture_home / ".config"),
                "XDG_CACHE_HOME": str(fixture_home / ".cache"), "TMPDIR": str(temporary)})
    return env


def execute(command, cwd, env, timeout, log_path):
    """Bound the whole child process group and reap it, including interruption."""
    with log_path.open("wb") as log:
        try:
            process = subprocess.Popen(command, cwd=cwd, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
        except OSError as error:
            log.write(str(error).encode("utf-8", errors="replace"))
            return None, "launch failure: " + str(error)
        try:
            process.wait(timeout=timeout)
            return process.returncode, None
        except subprocess.TimeoutExpired:
            return None, "timeout"
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def run_case(args, root, fixtures, name, state, default=False, model=None,
             project=False, conflicting_default=None, storage=None, support_kind=None):
    case = root / name
    case.mkdir()
    storage = storage or case
    fixture_home, temporary = storage / "isolated-home", case / "tmp"
    temporary.mkdir()
    datadir = storage / "datadir"
    set_preference(default_config_directory(fixture_home), state if default else conflicting_default)
    set_preference(datadir, state)
    command = [str(args.binary)]
    if not default:
        command += ["--datadir", str(datadir)]
    if not project:
        process = (support_kind + ".json") if support_kind else "process.json"
        command += ["--load-settings", str(fixtures / "machine.json") + ";" + str(fixtures / process),
                    "--load-filaments", str(fixtures / "filament.json")]
        model = model or fixtures / ("capital.stl" if support_kind else "cube.stl")
    command += ["--slice", "0", "--outputdir", str(case), "--export-3mf", "project.3mf", str(model)]
    environment = sanitized_env(fixture_home, temporary)
    write_json(case / "command.json", {"argv": command, "cwd": str(case),
                                      "timeout_seconds": args.timeout, "env": environment})
    started = dt.datetime.now(dt.timezone.utc).isoformat()
    try:
        status, reason = execute(command, case, environment, args.timeout, case / "console.log")
    finally:
        shutil.rmtree(temporary)
    # Preserve the application's result instead of overwriting it with this report.
    if (case / "result.json").is_file():
        (case / "result.json").rename(case / "cli-result.json")
    gcodes, sequences = [], []
    for path in sorted(case.rglob("*.gcode")):
        raw = path.read_bytes()
        analysis = summarize_gcode(raw.decode("utf-8", errors="replace"))
        sequences.extend(dict(sequence, gcode=str(path.relative_to(case)))
                         for sequence in analysis.pop("role_travel_sequences"))
        gcodes.append({"path": str(path.relative_to(case)), "sha256": digest(raw), **analysis})
    expected = 0 if state is True else 1
    passed = status == 0 and bool(gcodes) and all(
        set(item["reduce_infill_retraction_values"]) == {expected} for item in gcodes)
    support_extrusions = sum(sum(item["role_extrusion_moves"].get(role, 0) for role in SUPPORT_ROLES) for item in gcodes)
    result = {"name": name, "started_at": started, "returncode": status, "failure_reason": reason,
              "preference": state, "default_lookup": default, "expected_option": expected,
              "passed": passed, "gcode": gcodes, "project_reimport": project,
              "project_exists": (case / "project.3mf").is_file(), "support_kind": support_kind,
              "support_extrusion_moves": support_extrusions,
              "travel_sequence_report": "travel-sequences.json"}
    write_json(case / "travel-sequences.json", sequences)
    write_json(case / "result.json", result)
    print(name + ": " + ("PASS" if passed else "FAIL"), flush=True)
    return result


def case_sequences(root, result):
    return json.loads((root / result["name"] / "travel-sequences.json").read_text(encoding="utf-8"))


def compare_cases(root, off, on, roles, name):
    comparison = compare_travels(case_sequences(root, off), case_sequences(root, on), roles)
    matches = comparison.pop("matched_sequences")
    write_json(root / (name + "-matched-travels.json"), matches)
    comparison.update({"off_case": off["name"], "on_case": on["name"],
                       "matched_sequence_report": name + "-matched-travels.json"})
    return comparison


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    if sys.platform not in {"darwin", "linux"}:
        parser.error("default configuration lookup is supported on Linux and macOS")
    if args.timeout <= 0 or args.timeout > 1800:
        parser.error("--timeout must be between 1 and 1800 seconds per slice")
    args.binary = args.binary.resolve(strict=True)
    args.source = args.source.resolve(strict=True)
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error("--binary must name an existing executable")
    app_folder = args.binary.parent
    if sys.platform == "darwin":
        app_folder = app_folder.parent.parent.parent
    if (app_folder / "data_dir").exists():
        parser.error("portable data_dir overrides isolated default lookup; use a build without it")
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    fixtures = root / "fixtures"
    create_fixtures(fixtures)
    source_head = subprocess.run(["/usr/bin/git", "rev-parse", "HEAD"], cwd=args.source,
                                 capture_output=True, text=True, check=True, timeout=30).stdout.strip()
    results = []
    for default in (False, True):
        prefix = "default" if default else "explicit"
        for suffix, state in (("unset", None), ("off", False), ("on", True), ("off-again", False)):
            results.append(run_case(args, root, fixtures, prefix + "-" + suffix, state, default,
                                    storage=root / (prefix + "-state")))
    results.append(run_case(args, root, fixtures, "explicit-off-beats-default-on", False, conflicting_default=True))
    results.append(run_case(args, root, fixtures, "explicit-on-beats-default-off", True, conflicting_default=False))
    project_path = root / "explicit-off/project.3mf"
    project_value, project_error = None, None
    if project_path.is_file():
        try:
            with zipfile.ZipFile(project_path) as archive:
                project_value = json.loads(archive.read("Metadata/project_settings.config")).get("reduce_infill_retraction")
            if str(project_value).lower() not in {"1", "true"}:
                project_error = "exported fixture did not retain reduce_infill_retraction = 1"
            else:
                results.append(run_case(args, root, fixtures, "import-project-on", True, model=project_path, project=True))
                results.append(run_case(args, root, fixtures, "import-project-off", False, model=project_path, project=True))
        except (KeyError, ValueError, zipfile.BadZipFile) as error:
            project_error = str(error)
    else:
        project_error = "explicit-off did not produce project.3mf"
    by_name = {result["name"]: result for result in results}
    infill = compare_cases(root, by_name["explicit-off"], by_name["explicit-on"], INFILL_ROLES, "infill")
    support_results, support_comparisons = [], {}
    for kind in ("normal", "tree"):
        off = run_case(args, root, fixtures, kind + "-support-off", False, support_kind=kind)
        on = run_case(args, root, fixtures, kind + "-support-on", True, support_kind=kind)
        support_results.extend([off, on])
        comparison = compare_cases(root, off, on, SUPPORT_ROLES, kind + "-support")
        comparison["coverage_observed"] = (off["passed"] and on["passed"] and off["support_extrusion_moves"] > 0
                                            and on["support_extrusion_moves"] > 0 and comparison["matched_travels"] > 0)
        comparison["verdict"] = "diagnostic only; no defect or regression asserted"
        support_comparisons[kind] = comparison
    config_passed = len(results) == 12 and all(case["passed"] for case in results) and project_error is None
    infill_observed = (infill["retraction_added_with_preference_on"] > 0
                       and infill["returned_z_hop_added_with_preference_on"] > 0)
    summary = {
        "source_checkout_head": source_head, "binary": str(args.binary), "platform": sys.platform,
        "binary_sha256": digest(args.binary.read_bytes()),
        "source_limit": "Checkout head and binary hash are separate; retain the build log to establish build provenance.",
        "fixture_sha256": {path.name: digest(path.read_bytes()) for path in sorted(fixtures.iterdir())},
        "reimport_source_project_value": project_value, "project_error": project_error,
        "configuration_cases": results, "configuration_matrix_passed": config_passed,
        "infill_motion_comparison": infill, "infill_motion_observed": infill_observed,
        "support_cases": support_results, "support_diagnostics": support_comparisons,
        "passed": config_passed and infill_observed,
        "limitations": [
            "No GUI operations, physical prints or printer connections.",
            "Fresh child processes check configuration reads; GUI save/relaunch needs separate verification.",
            "Matched G-code establishes role, endpoints and E/Z sequences, not internal/support-island containment.",
            "Existing support-island/tree early returns precede reduce_infill_retraction; support results alone do not establish a PR regression.",
            "Normal/tree support diagnostics do not affect the 12-case configuration/infill gate.",
        ],
    }
    write_json(root / "summary.json", summary)
    print(str(root / "summary.json"), flush=True)
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
