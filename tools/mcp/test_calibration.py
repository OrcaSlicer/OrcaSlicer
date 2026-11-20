#!/usr/bin/env python3
"""The Calibration menu's tests through calibration_generate_test, as the test_calibration entry of
tools/mcp/test-entries/presets.md designs them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_calibration.py

Every test and method: the project it makes, the parameters it uses, the presets and objects it changes, the warnings
for values it rounds or the printer may not take, and the refusals that leave the project and presets as they were;
preset_changes "keep" and "discard", including the filament tab on another slot's modified preset; and, through the
exported G-code, that the temperature, pressure advance and retraction towers change their value band by band. After
each call the test reads the project, the objects and the presets back with other tools and compares them with the
values the entry writes out. It runs on the default printer, the Generic Klipper Printer 0.4, on a data folder earlier
tests used, and exits 0 when every check passes.
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import GENERIC_PLA, KLIPPER, KLIPPER_STD, Checks, refusal, run, start  # noqa: E402

STD, PLA = KLIPPER_STD, GENERIC_PLA
SHORT = {"Klipper": KLIPPER, "Std": STD, "PLA": PLA}
START = {"printer": KLIPPER, "process": STD, "filaments": [PLA]}
# The user filament keep and discard saves, which a run that failed before its step 6 leaves.
CALIB_PLA = "MCP Calib PLA"
DISCARD = {"discard_changes": True, "preset_changes": "discard"}
FILAMENT_ADD = "orca_command:filament_add:orca"
TEMPERATURE_KEYS = ["nozzle_temperature", "nozzle_temperature_initial_layer"]


class Every:
    """A value with one entry per extruder or variant, each entry `value`: Every("0") equals "0,0"."""

    def __init__(self, value):
        self.value = str(value)

    def __eq__(self, other):
        if not isinstance(other, str) or not other:
            return False
        return all(same_number(entry, self.value) for entry in other.split(","))

    def __repr__(self):
        return f"every entry {self.value}"


def times(value, n):
    """The entry's `v×n`: n entries, each v, joined by commas. PLA holds six entries of each per-variant key."""
    return ",".join([str(value)] * n)


def same_number(a, b):
    try:
        return float(a) == float(b)
    except ValueError:
        return a == b


def split_top(text):
    """`text` split at the ", " that stand outside brackets."""
    parts, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        depth += {"[": 1, "]": -1}.get(ch, 0)
        if depth == 0 and text.startswith(", ", i):
            parts.append(text[start:i])
            start = i + 2
    parts.append(text[start:])
    return parts


def S(text):
    """A selection as the entry writes it: 'Klipper | Std | PLA, MCP Calib PLA[filament_cost 22]' is the Klipper,
    Std, and two filament slots, the second's preset with filament_cost edited to 22. Only the filaments' edits are
    kept: the test compares the printer and the process by name."""
    printer, process, filaments = text.split(" | ")

    def preset(s):
        m = re.match(r"^(.*?)(?:\[(.*)\])?$", s)
        edits = {}
        if m.group(2):
            for pair in split_top(m.group(2)):
                key, _, value = pair.partition(" ")
                edits[key] = value.strip('"')
        return SHORT.get(m.group(1), m.group(1)), edits

    return {"printer": preset(printer)[0], "process": preset(process)[0],
            "filaments": [dict(zip(("name", "edits"), preset(f))) for f in split_top(filaments)]}


def pla(edits):
    """The selection `Klipper | Std | PLA` with PLA's unsaved keys and values `edits`."""
    return {"printer": KLIPPER, "process": STD, "filaments": [{"name": PLA, "edits": dict(edits)}]}


def gcode_layers(path):
    """The lines of an exported G-code file after its first `;LAYER_CHANGE` line, the line the Klipper's G-code
    writes at each layer change."""
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    first = next(i for i, line in enumerate(lines) if line.startswith(";LAYER_CHANGE"))
    return lines[first + 1:]


def in_a_row(values):
    """`values` with repeats in a row dropped."""
    return [v for i, v in enumerate(values) if i == 0 or v != values[i - 1]]


def flow_offset(name):
    """The number a flow ratio object's name ends in, "m" for minus: flowrate_m0.01 is -0.01."""
    m = re.fullmatch(r"flowrate_(m?)([0-9.]+)", name)
    assert m, name
    return -float(m.group(2)) if m.group(1) else float(m.group(2))


class CalibrationTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.folder = folder

    # ---- Calls and reads ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def value(self, type_, key, preset=None, slot=None):
        args = {"type": type_, "keys": [key], "include_meta": False}
        if preset is not None:
            args["preset"] = preset
        if slot is not None:
            args["filament_slot"] = slot
        return self.c.call("settings_get", **args)["items"][0]["value"]

    def selection(self):
        """The printer, the process and each slot's filament by name, with the unsaved keys of each slot's filament
        and their whole values."""
        presets = self.c.call("app_state")["presets"]
        filaments = []
        for slot, name in enumerate(presets["filaments"], 1):
            keys = []
            for item in self.c.call("settings_get", type="filament", compare=":saved", filament_slot=slot,
                                    limit=500)["items"]:
                if item["key"] not in keys:
                    keys.append(item["key"])
            filaments.append({"name": name, "edits": {k: self.value("filament", k, slot=slot) for k in keys}})
        return {"printer": presets["printer"], "process": presets["process"], "filaments": filaments}

    def objects(self):
        listing = self.c.call("object_list", limit=500)
        return [{"name": o["name"], "height": o["bbox"]["size"][2]} for p in listing["plates"] for o in p["objects"]]

    def own(self, obj, key):
        return self.c.call("settings_get", target="object", object=obj, keys=[key])["items"][0]["value"]

    def listed(self, **filters):
        """Every filament preset_list gives for the filters, page after page."""
        items = []
        while True:
            page = self.c.call("preset_list", type="filament", limit=500, offset=len(items), **filters)
            items += page["items"]
            if not page["items"] or len(items) >= page["total"]:
                return items

    def project_name(self):
        return self.c.call("app_state")["project"]["name"]

    def selection_is(self, label, want):
        self.t.equal(f"{label}: the selection", self.selection(), want)

    def refused(self, label, call, code=None, words=None):
        """Checks.refused, giving the refusal's details as well as its text."""
        caught = {}

        def wrapped():
            try:
                return call()
            except orca_mcp.McpError as e:
                caught["text"], caught["details"] = refusal(e)
                raise

        self.t.refused(label, wrapped, code=code, words=words)
        return caught.get("text", ""), caught.get("details", {})

    def slice(self, label, export=None):
        """plate_slice and a check that it ended ok; with `export`, the G-code written to that new file in the test's
        folder, whose path it returns."""
        reply = self.run("plate_slice")
        self.t.equal(f"{label}: the slice", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        if export is None:
            return None
        path = os.path.join(self.folder, export)
        self.run("project_export", format="gcode", path=path)
        return path

    def generate(self, label, args, params, project=None, names=None, heights=None, changes=None, sets=None,
                 result_keys=None):
        """Makes the test with `args`, discarding the project's and the presets' changes, and checks what the row
        fixes: the reply's params whole; project, read back from app_state; names, read back from object_list, with
        their heights to 0.01 where given; the filament's changes whole in the reply and in the selection; each key
        `sets` lists, read with value(); and result_keys."""
        t = self.t
        reply = self.run("calibration_generate_test", **DISCARD, **args)
        if params is not None:
            t.equal(f"{label}: params", reply.get("params"), params)
        if project is not None:
            t.equal(f"{label}: the reply's project", reply.get("project"), project)
            t.equal(f"{label}: the project's name", self.project_name(), project)
        if names is not None:
            t.equal(f"{label}: the reply's names", reply.get("names"), names)
            objects = self.objects()
            t.equal(f"{label}: the objects", [o["name"] for o in objects], names)
            if heights is not None:
                t.near(f"{label}: the objects' heights", [o["height"] for o in objects], heights, 0.01)
        if changes is not None:
            filament = reply.get("presets_changed", {}).get("filament", {})
            t.equal(f"{label}: the filament's changes", {k: filament.get(k) for k in ("name", "changes")},
                    {"name": PLA, "changes": changes})
            self.selection_is(label, pla(changes))
        for (type_, key), want in (sets or {}).items():
            t.equal(f"{label}: sets {type_} {key}", self.value(type_, key), want)
        if result_keys is not None:
            t.equal(f"{label}: result_keys", reply.get("result_keys"), result_keys)
        return reply

    # ---- Scenarios ----

    def temperature(self):
        s = "temperature"
        t = self.t
        t.equal(f"{s} 1: the start", start(self.c), START)
        t.equal(f"{s} 1: no objects", self.objects(), [])

        reply = self.run("calibration_generate_test", test="temperature", start=252, end=232)
        t.equal(f"{s} 2: the reply's project", reply.get("project"), "Nozzle temperature test")
        t.equal(f"{s} 2: params", reply.get("params"), {"start": 250, "end": 230, "auto_scale": True})
        t.equal(f"{s} 2: warnings", reply.get("warnings"),
                ["start 252 °C rounded down to 250 °C, the tower's 5 °C blocks",
                 "end 232 °C rounded down to 230 °C, the tower's 5 °C blocks"])
        t.equal(f"{s} 2: the reply's names", reply.get("names"), ["temperature_tower.drc"])
        filament = reply.get("presets_changed", {}).get("filament", {})
        t.equal(f"{s} 2: the filament's changes", {k: filament.get(k) for k in ("name", "changes")},
                {"name": PLA, "changes": {"nozzle_temperature": times(250, 6),
                                          "nozzle_temperature_initial_layer": times(250, 6)}})
        t.equal(f"{s} 2: result_keys", reply.get("result_keys"), {"type": "filament", "keys": TEMPERATURE_KEYS})
        t.equal(f"{s} 2: modified", reply.get("modified", {}).get("filament", {}).get("keys"), TEMPERATURE_KEYS)
        t.equal(f"{s} 2: the project is dirty", self.c.call("app_state")["project"]["dirty"], True)
        after2 = S(f"Klipper | Std | PLA[nozzle_temperature {times(250, 6)}, "
                   f"nozzle_temperature_initial_layer {times(250, 6)}]")

        def as_after_2(label):
            t.equal(f"{label}: the project's name", self.project_name(), "Nozzle temperature test")
            objects = self.objects()
            t.equal(f"{label}: the objects", [o["name"] for o in objects], ["temperature_tower.drc"])
            t.near(f"{label}: the tower's height, five 10 mm blocks", [o["height"] for o in objects], [50], 0.01)
            t.equal(f"{label}: the tower's own layer height", self.own(0, "layer_height"), "0.2")
            self.selection_is(label, after2)

        as_after_2(f"{s} 2")

        path = self.slice(f"{s} 3", "temperature.gcode")
        temps = [float(m.group(1)) for line in gcode_layers(path)
                 if (m := re.match(r"M104 S([0-9.]+)", line))]
        t.equal(f"{s} 3: the tower's temperatures, block by block", in_a_row([v for v in temps if v != 0]),
                [250, 245, 240, 235, 230])

        text, details = self.refused(f"{s} 4: refused while the presets are modified",
                                  lambda: self.c.call("calibration_generate_test", test="max_volumetric",
                                                      discard_changes=True),
                                  words="Presets are modified")
        t.check(f"{s} 4: the refusal names preset_changes", "preset_changes" in text, text)
        t.equal(f"{s} 4: the refusal's modified keys", details.get("modified", {}).get("filament", {}).get("keys"),
                TEMPERATURE_KEYS)
        as_after_2(f"{s} 4")

        _, details = self.refused(f"{s} 5: an argument the test does not take is refused",
                                  lambda: self.c.call("calibration_generate_test", test="temperature", step=5,
                                                      **DISCARD),
                                  words="step does not apply to temperature")
        t.equal(f"{s} 5: the arguments it takes", details.get("takes"), ["start", "end", "auto_scale", "test_model"])
        as_after_2(f"{s} 5")

        self.refused(f"{s} 6: a step past the range is refused",
                     lambda: self.c.call("calibration_generate_test", test="pressure_advance", start=0.01, end=0.05,
                                         step=0.051, **DISCARD),
                     words="pressure_advance needs start >= 0, step >= 0.001 and end >= start + step; "
                           "got start 0.01, end 0.05, step 0.051")
        as_after_2(f"{s} 6")

        self.refused(f"{s} 7: refused without discard_changes",
                     lambda: self.c.call("calibration_generate_test", test="retraction", preset_changes="discard"),
                     code="unsaved_changes")
        as_after_2(f"{s} 7")

    def other_tests(self):
        s = "other tests"
        t = self.t
        self.generate(f"{s}: the start", {"test": "temperature"}, {"start": 230, "end": 190, "auto_scale": True},
                      changes={"nozzle_temperature": times(230, 6), "nozzle_temperature_initial_layer": times(230, 6)})

        label = f"{s} 1"
        reply = self.generate(label, {"test": "max_volumetric", "end": 15}, {"start": 5, "end": 15, "step": 0.5},
                              project="Max volumetric speed test", names=["SpeedTestStructure.drc"], heights=[22],
                              changes={"filament_max_volumetric_speed": times(200, 6), "slow_down_layer_time": "0"},
                              sets={("filament", "filament_max_volumetric_speed"): times(200, 6),
                                    ("filament", "slow_down_layer_time"): "0", ("process", "spiral_mode"): "1"},
                              result_keys={"type": "filament", "keys": ["filament_max_volumetric_speed"]})
        t.equal(f"{label}: discarded", reply.get("discarded", {}).get("filament"), {"name": PLA, "keys": TEMPERATURE_KEYS})
        t.equal(f"{label}: the temperature is the preset's again", self.value("filament", "nozzle_temperature"),
                times(220, 6))
        self.slice(label)

        label = f"{s} 2"
        self.generate(label, {"test": "pressure_advance", "end": 0.06},
                      {"method": "tower", "start": 0, "end": 0.06, "step": 0.002}, project="Pressure Advance Test",
                      names=["tower_with_seam.drc"], changes={"slow_down_layer_time": "1"},
                      sets={("filament", "slow_down_layer_time"): "1"},
                      result_keys={"type": "filament", "keys": ["enable_pressure_advance", "pressure_advance"]})
        path = self.slice(label, "pressure_advance.gcode")
        # A Klipper printer's pressure advance command.
        ks = [float(m.group(1)) for line in gcode_layers(path)
              if (m := re.match(r"SET_PRESSURE_ADVANCE ADVANCE=([0-9.]+)", line))]
        t.near(f"{label}: the tower's pressure advance, band by band", in_a_row(ks),
               [round(0.002 * i, 6) for i in range(31)], 0.0001)

        self.generate(f"{s} 3", {"test": "pressure_advance", "method": "line", "print_numbers": False},
                      {"method": "line", "start": 0, "end": 0.1, "step": 0.002, "print_numbers": False},
                      project="Pressure Advance Test", names=["pressure_advance_test.drc"], changes={},
                      sets={("process", "wipe_inward"): "0", ("process", "retract_after_outer_wall"): "0"})

        pattern_changes = {"filament_retract_when_changing_layer": Every("0"), "filament_wipe": Every("0")}
        pattern_sets = {("filament", "filament_retract_when_changing_layer"): Every("0"),
                        ("filament", "filament_wipe"): Every("0"), ("printer", "wipe"): Every("0"),
                        ("printer", "retract_when_changing_layer"): Every("0")}
        label = f"{s} 4"
        self.generate(label, {"test": "pressure_advance", "method": "pattern", "accelerations": [1000, 2000],
                              "speeds": [50, 100]},
                      {"method": "pattern", "start": 0, "end": 0.08, "step": 0.005, "accelerations": [1000, 2000],
                       "speeds": [50, 100]},
                      project="Pressure Advance Test",
                      names=["pa_pattern_50_1000", "pa_pattern_100_1000", "pa_pattern_50_2000", "pa_pattern_100_2000"],
                      changes=pattern_changes, sets=pattern_sets,
                      result_keys={"type": "filament", "keys": ["enable_pressure_advance", "pressure_advance",
                                                                "adaptive_pressure_advance",
                                                                "adaptive_pressure_advance_model"]})
        self.slice(label)

        # The pattern's defaults are the first entries of Std's outer_wall_acceleration and outer_wall_speed.
        self.generate(f"{s} 5", {"test": "pressure_advance", "method": "pattern"},
                      {"method": "pattern", "start": 0, "end": 0.08, "step": 0.005, "accelerations": [3000],
                       "speeds": [120]},
                      project="Pressure Advance Test", names=["pa_pattern_120_3000"], changes=pattern_changes,
                      sets=pattern_sets)

        label = f"{s} 6"
        reply = self.generate(label, {"test": "flow_ratio"}, {"method": "yolo", "top_surface_pattern": "archimedeanchords"},
                              project="Orca YOLO Flow Calibration",
                              names=["flowrate_0", "flowrate_0.01", "flowrate_0.02", "flowrate_0.03", "flowrate_0.04",
                                     "flowrate_0.05", "flowrate_m0.01", "flowrate_m0.02", "flowrate_m0.03",
                                     "flowrate_m0.04", "flowrate_m0.05"],
                              changes={}, sets={("process", "layer_height"): "0.2"},
                              result_keys={"type": "filament", "keys": ["filament_flow_ratio"]})
        self.flow_ratios(label, lambda m: (0.98 + m) / 0.98, "archimedeanchords")
        self.slice(label)

        label = f"{s} 7"
        self.generate(label, {"test": "flow_ratio", "method": "yolo_perfectionist"},
                      {"method": "yolo_perfectionist", "top_surface_pattern": "archimedeanchords"},
                      project="Orca YOLO Flow Calibration - Perfectionist version",
                      names=["flowrate_0", "flowrate_0.005", "flowrate_0.01", "flowrate_0.015", "flowrate_0.02",
                             "flowrate_0.025", "flowrate_0.03", "flowrate_0.035", "flowrate_m0.005", "flowrate_m0.01",
                             "flowrate_m0.015", "flowrate_m0.02", "flowrate_m0.025", "flowrate_m0.03", "flowrate_m0.035",
                             "flowrate_m0.04"],
                      changes={})
        self.flow_ratios(label, lambda m: (0.98 + m) / 0.98)

        label = f"{s} 8"
        self.generate(label, {"test": "flow_ratio", "method": "pass1"},
                      {"method": "pass1", "top_surface_pattern": "archimedeanchords"}, project="Flowrate Test - Pass1",
                      names=["flowrate_0", "flowrate_10", "flowrate_15", "flowrate_20", "flowrate_5", "flowrate_m10",
                             "flowrate_m15", "flowrate_m20", "flowrate_m5"],
                      changes={})
        self.flow_ratios(label, lambda m: 1 + m / 100)

        label = f"{s} 9"
        self.generate(label, {"test": "flow_ratio", "method": "pass2", "top_surface_pattern": "monotonic"},
                      {"method": "pass2", "top_surface_pattern": "monotonic"}, project="Flowrate Test - Pass2",
                      names=["flowrate_0"] + [f"flowrate_m{i}" for i in range(1, 10)],
                      changes={})
        self.flow_ratios(label, lambda m: 1 + m / 100, "monotonic")

        label = f"{s} 10"
        self.generate(label, {"test": "retraction", "end": 1}, {"start": 0, "end": 1, "step": 0.1}, project="Retraction",
                      names=["retraction_tower.drc"], changes={}, sets={("printer", "use_firmware_retraction"): "0"},
                      result_keys={"type": "filament", "keys": ["filament_retraction_length"]})
        t.equal(f"{label}: the tower's own layer height", self.own(0, "layer_height"), "0.2")
        path = self.slice(label, "retraction.gcode")
        self.retraction_bands(label, path)

        # One row for each way the VFA tower fits its end speed to the filament's volumetric speed (12 mm³/s on the
        # extruder variant in use), which allows the outer wall about 168 mm/s at 0.2 mm.
        vfa = {"project": "VFA test", "names": ["vfa.drc"], "changes": {"slow_down_layer_time": "0"},
               "sets": {("filament", "slow_down_layer_time"): "0", ("process", "spiral_mode"): "1",
                        ("process", "wall_loops"): "1", ("process", "top_shell_layers"): "0",
                        ("process", "bottom_shell_layers"): "1", ("process", "sparse_infill_density"): "0%",
                        ("process", "enable_overhang_speed"): Every("0")},
               "result_keys": {"type": "printer", "keys": ["resonance_avoidance", "min_resonance_avoidance_speed",
                                                           "max_resonance_avoidance_speed"]}}
        label = f"{s} 11"
        reply = self.generate(label, {"test": "vfa", "end": 160},
                              {"start": 40, "end": 160, "step": 10, "auto_scale": True, "auto_adjust": True}, **vfa)
        t.equal(f"{label}: no adjusted", reply.get("adjusted"), None)
        t.equal(f"{label}: the tower's own layer height", self.own(0, "layer_height"), "0.2")

        label = f"{s} 12"
        reply = self.generate(label, {"test": "vfa"},
                              {"start": 40, "end": 200, "step": 10, "auto_scale": True, "auto_adjust": True}, **vfa)
        t.equal(f"{label}: adjusted", reply.get("adjusted"),
                {"layer_height": 0.16,
                 "reason": "the end speed (200 mm/s) exceeds the filament's maximum volumetric speed (12 mm³/s) at the "
                           "default layer height (0.2 mm); the layer height of a profile of this printer reaches it"})
        t.equal(f"{label}: the tower's own layer height", self.own(0, "layer_height"), "0.16")
        self.slice(label)

        label = f"{s} 13"
        reply = self.generate(label, {"test": "vfa", "end": 400, "step": 20},
                              {"start": 40, "end": 380, "step": 20, "auto_scale": True, "auto_adjust": True}, **vfa)
        t.equal(f"{label}: adjusted", reply.get("adjusted"),
                {"end": 380, "layer_height": 0.08,
                 "reason": "the end speed (400 mm/s) exceeds the filament's maximum volumetric speed (12 mm³/s) even at "
                           "the smallest layer height of this printer's profiles; the end speed was lowered to what it "
                           "reaches"})
        t.equal(f"{label}: the tower's own layer height", self.own(0, "layer_height"), "0.08")

        label = f"{s} 14"
        reply = self.generate(label, {"test": "vfa", "end": 400, "auto_adjust": False},
                              {"start": 40, "end": 400, "step": 10, "auto_scale": True, "auto_adjust": False}, **vfa)
        t.equal(f"{label}: no adjusted", reply.get("adjusted"), None)
        t.equal(f"{label}: warnings", reply.get("warnings"),
                ["the end speed (400 mm/s) exceeds the filament's maximum volumetric speed (12 mm³/s), which limits "
                 "the outer wall to about 168 mm/s: the blocks above print at that speed (auto_adjust and auto_scale "
                 "lower the layer height to reach it)"])
        t.equal(f"{label}: the tower's own layer height, half the nozzle", self.own(0, "layer_height"), "0.2")

        # The outer wall speed is the smaller of the Klipper's machine_max_speed_x and _y, and the accelerations its
        # machine_max_acceleration_extruding; the jerk is raised to at least 5, below the Klipper's own 9.
        shaping = {"changes": {"slow_down_layer_time": "0", "slow_down_min_speed": "0", "slow_down_for_layer_cooling": "0",
                               "enable_pressure_advance": Every("1"), "pressure_advance": Every("0")},
                   "sets": {("filament", "slow_down_layer_time"): "0", ("filament", "slow_down_min_speed"): "0",
                            ("filament", "slow_down_for_layer_cooling"): "0",
                            ("filament", "enable_pressure_advance"): Every("1"),
                            ("filament", "pressure_advance"): Every("0"),
                            ("printer", "machine_max_jerk_x"): "9", ("printer", "machine_max_jerk_y"): "9",
                            ("printer", "input_shaping_emit"): "0",
                            ("process", "spiral_mode"): "1", ("process", "wall_loops"): "1",
                            ("process", "top_shell_layers"): "0", ("process", "bottom_shell_layers"): "1",
                            ("process", "sparse_infill_density"): "0%", ("process", "enable_overhang_speed"): Every("0"),
                            ("process", "accel_to_decel_enable"): "1", ("process", "accel_to_decel_factor"): "100%",
                            ("process", "outer_wall_speed"): Every(500),
                            ("process", "default_acceleration"): Every(20000),
                            ("process", "outer_wall_acceleration"): Every(20000)}}
        label = f"{s} 15"
        self.generate(label, {"test": "input_shaping_freq", "start": 20, "end": 80},
                      {"start": 20, "end": 80, "start_y": 20, "end_y": 80, "damping": 0.15, "shaper_type": "Default",
                       "test_model": "ringing_tower"},
                      project="Input shaping Frequency test", names=["ringing_tower.drc"],
                      result_keys={"type": "printer", "keys": ["input_shaping_emit", "input_shaping_type",
                                                               "input_shaping_freq_x", "input_shaping_freq_y",
                                                               "input_shaping_damp_x", "input_shaping_damp_y"]},
                      **shaping)
        self.slice(label)

        label = f"{s} 16"
        self.generate(label, {"test": "input_shaping_damp", "freq_x": 48, "freq_y": 42, "test_model": "fast_tower"},
                      {"freq_x": 48, "freq_y": 42, "start": 0, "end": 0.4, "shaper_type": "Default",
                       "test_model": "fast_tower"},
                      project="Input shaping Damping test", names=["fast_tower_test.drc"], **shaping)
        self.slice(label)

        label = f"{s} 17"
        reply = self.generate(
            label, {"test": "cornering", "test_model": "scv_v2", "end": 25}, {"start": 1, "end": 25, "test_model": "scv_v2"},
            project="Cornering test", names=["SCV-V2.drc"],
            changes={"slow_down_layer_time": "0", "slow_down_min_speed": "0", "slow_down_for_layer_cooling": "0",
                     "filament_max_volumetric_speed": "200", "enable_pressure_advance": Every("1"),
                     "pressure_advance": Every("0")},
            sets={("printer", "machine_max_jerk_x"): "25", ("printer", "machine_max_jerk_y"): "25",
                  ("printer", "input_shaping_emit"): "1", ("printer", "input_shaping_type"): "Disable",
                  ("filament", "slow_down_layer_time"): "0", ("filament", "slow_down_min_speed"): "0",
                  ("filament", "slow_down_for_layer_cooling"): "0", ("filament", "filament_max_volumetric_speed"): "200",
                  ("filament", "enable_pressure_advance"): Every("1"), ("filament", "pressure_advance"): Every("0")},
            result_keys={"type": "printer", "keys": ["machine_max_jerk_x", "machine_max_jerk_y"]})
        t.equal(f"{label}: warnings", reply.get("warnings"), ["end 25 is above 20: high values may cause layer shifts"])
        self.slice(label)

    def flow_ratios(self, label, ratio, pattern=None):
        """Each object's own print_flow_ratio, `ratio` of the number its name ends in, and with `pattern` its own
        top_surface_pattern."""
        objects = self.objects()
        got = [float(self.own(i, "print_flow_ratio")) for i in range(len(objects))]
        self.t.near(f"{label}: each object's own flow ratio", got, [ratio(flow_offset(o["name"])) for o in objects],
                    0.0001)
        if pattern is not None:
            self.t.equal(f"{label}: each object's own top surface pattern",
                         [self.own(i, "top_surface_pattern") for i in range(len(objects))], [pattern] * len(objects))

    def retraction_bands(self, label, path):
        """The layers cut into bands at each `; Calib_Retraction_tower: ...` line: the bands' lengths in a row, and the
        lengths each length's bands restore."""
        bands = []
        for line in gcode_layers(path):
            m = re.match(r"; Calib_Retraction_tower: Z_HEIGHT: [0-9.]+, length:([0-9.]+)", line)
            if m:
                bands.append((float(m.group(1)), []))
                continue
            words = line.split(";")[0].split()
            if bands and len(words) == 3 and words[0] == "G1" and sorted(w[0] for w in words[1:]) == ["E", "F"]:
                e = float(next(w for w in words[1:] if w[0] == "E")[1:])
                if e > 0:
                    bands[-1][1].append(e)
        lengths = [round(0.1 * i, 6) for i in range(11)]
        self.t.near(f"{label}: the tower's retraction lengths, band by band", in_a_row([b[0] for b in bands]), lengths,
                    1e-9)
        restored = {}
        for length, deretractions in bands:
            restored.setdefault(round(length, 3), set()).update(round(e, 3) for e in deretractions)
        want = {length: {length} for length in lengths}
        want[0.0] = {0.8}  # the first layer's retraction, made before the tower's first line
        self.t.equal(f"{label}: the lengths each band's deretractions restore",
                     {k: sorted(v) for k, v in restored.items()}, {k: sorted(v) for k, v in want.items()})

    def keep_and_discard(self):
        s = "keep and discard"
        t = self.t
        start(self.c)
        reply = self.run("action_run", id=FILAMENT_ADD)
        t.equal(f"{s} 1: the action's message", reply.get("message"), "Added filament slot 2.")
        self.run("settings_set", type="filament", filament_slot=2, values={"filament_cost": 21})
        self.run("preset_save", type="filament", name=CALIB_PLA, filament_slot=2)
        self.run("preset_select", type="filament", name=PLA, filament_slot=1)
        self.run("settings_set", type="filament", filament_slot=2, values={"filament_cost": 22})
        after1 = S("Klipper | Std | PLA, MCP Calib PLA[filament_cost 22]")
        self.selection_is(f"{s} 1", after1)
        saved_cost = lambda: self.value("filament", "filament_cost", preset=CALIB_PLA)  # noqa: E731
        t.equal(f"{s} 1: the saved cost", saved_cost(), "21")

        self.refused(f"{s} 2: keep is refused while the tab edits another slot's modified preset",
                     lambda: self.c.call("calibration_generate_test", test="retraction", discard_changes=True,
                                         preset_changes="keep"),
                     words='The filament tab edits "MCP Calib PLA", which has unsaved changes, and the test prints '
                           f'with filament slot 1 ("{PLA}")')
        self.selection_is(f"{s} 2", after1)
        t.equal(f"{s} 2: no objects", self.objects(), [])
        t.equal(f"{s} 2: the saved cost", saved_cost(), "21")

        reply = self.run("calibration_generate_test", test="retraction", **DISCARD)
        filament = reply.get("presets_changed", {}).get("filament", {})
        t.equal(f"{s} 3: the filament tab switched to slot 1",
                {k: filament.get(k) for k in ("name", "tab_switched_from")},
                {"name": PLA, "tab_switched_from": "MCP Calib PLA"})
        t.equal(f"{s} 3: discarded", reply.get("discarded", {}).get("filament"),
                {"name": "MCP Calib PLA", "keys": ["filament_cost"]})
        t.equal(f"{s} 3: the reply's names", reply.get("names"), ["retraction_tower.drc"])
        t.equal(f"{s} 3: the objects", [o["name"] for o in self.objects()], ["retraction_tower.drc"])
        self.selection_is(f"{s} 3", S("Klipper | Std | PLA, MCP Calib PLA"))
        t.equal(f"{s} 3: slot 2's cost", self.value("filament", "filament_cost", slot=2), "21")

        self.run("settings_set", type="filament", values={"slow_down_layer_time": 7})
        reply = self.run("calibration_generate_test", test="retraction", discard_changes=True, preset_changes="keep")
        t.equal(f"{s} 4: modified", reply.get("modified", {}).get("filament", {}).get("keys"), ["slow_down_layer_time"])
        t.equal(f"{s} 4: nothing discarded", reply.get("discarded"), None)
        self.selection_is(f"{s} 4", S("Klipper | Std | PLA[slow_down_layer_time 7], MCP Calib PLA"))

        reply = self.run("project_new", **DISCARD)
        t.check(f"{s} 5: discarded holds slow_down_layer_time",
                "slow_down_layer_time" in reply.get("discarded", {}).get("filament", {}).get("keys", []), reply)
        t.equal(f"{s} 5: nothing modified", reply.get("modified"), None)
        self.selection_is(f"{s} 5", S("Klipper | Std | PLA, MCP Calib PLA"))
        t.equal(f"{s} 5: slow_down_layer_time", self.value("filament", "slow_down_layer_time"), "4")

        reply = self.run("filament_delete", filament_slot=2)
        t.equal(f"{s} 6: the slot's delete", reply.get("level"), "success")
        reply = self.run("preset_delete", type="filament", name=CALIB_PLA)
        t.equal(f"{s} 6: the preset's delete", reply.get("deleted"), CALIB_PLA)
        self.selection_is(f"{s} 6", S("Klipper | Std | PLA"))
        # Read by name: the filaments listed besides hold those earlier tests installed or saved.
        listed = {i["name"]: i for i in self.listed()}
        t.equal(f"{s} 6: PLA listed not modified, MCP Calib PLA not listed",
                (PLA in listed, (listed.get(PLA) or {}).get("modified"), CALIB_PLA in listed), (True, None, False))

    def setup(self):
        """The start, and no "MCP Calib PLA" on the folder: a run that failed before keep and discard step 6 leaves
        it, and step 1's save, made without overwrite, would be refused."""
        start(self.c)
        if CALIB_PLA in [i["name"] for i in self.listed(query=CALIB_PLA)]:
            print(f"     (deleting the filament {CALIB_PLA!r} left on the folder)", flush=True)
            self.run("preset_delete", type="filament", name=CALIB_PLA)

    def run_all(self):
        self.t.scenario("the start", self.setup)
        for name, scenario in [("the temperature tower, and what is refused", self.temperature),
                               ("the other tests", self.other_tests), ("keep and discard", self.keep_and_discard)]:
            self.t.scenario(name, scenario)
        self.run("project_new", **DISCARD)
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-calibration-") as folder:
        return CalibrationTest(client, folder).run_all()


if __name__ == "__main__":
    sys.exit(main())
