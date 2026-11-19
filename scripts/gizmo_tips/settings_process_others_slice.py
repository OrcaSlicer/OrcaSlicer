"""The toolpaths behind the pictures of the Process tab's Multimaterial and Others pages
(settings_process_others.py), sliced by OrcaSlicer itself.

Works like settings_process_slice.py, whose test models and presets it reuses, with what these two pages
need on top: several objects on the plate, each with a filament of its own, objects merged into one with
--assemble (for the interlocking between two filaments), a second filament, and the tool each extrusion
was printed with. The extrusions it keeps are in settings_process_others_toolpaths.json.gz beside this file.

    python3 -m gizmo_tips.settings_process_others_slice <path to orca-slicer> [case ...]   # from scripts/
    python3 scripts/gen_gizmo_tips.py settings_process_others
"""

import gzip
import json
import math
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

from .settings_process_slice import CX, CY, MACHINE, PROCESS, _simplified, _write_stl, box, prism, ring

OUT = "settings/process"  # shares the folder of settings_process_others.py; draws nothing itself
FRAMES = {}

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "settings_process_others_toolpaths.json.gz")

# One extruder that switches between filaments, the most common multi-material setup.
MM_MACHINE = dict(MACHINE, single_extruder_multi_material="1")


def _filament(i):
    # The CLI merges every filament's keys into the print config and stops when one is missing, so the
    # filaments name the few it checks.
    return {"type": "filament", "from": "User", "name": "tips filament %d" % i, "inherits": "",
            "filament_colour": ["#808080"], "filament_diameter": ["1.75"], "filament_type": ["PLA"],
            "filament_settings_id": ["tips filament %d" % i], "filament_is_support": ["0"]}


def cylinder(x, y, r, z0, z1, segments=72):
    return list(ring(x, y, 0, r, z0, z1, segments))


# --- the cases ------------------------------------------------------------------------------------
#
# objects: [(triangles, filament number)]; machine, filaments (count), assemble: how the plate is set up;
# base: the process settings all values share; values: [(name, {settings})]; keep(z, type, tool): which
# extrusions the pictures need; flow: keep each segment's width as worked out from the filament it
# extrudes (for the settings that change the flow and not the line), and the path unsimplified.

TOWER = {"enable_prime_tower": "1", "outer_wall_filament_id": "2", "wall_loops": "2", "sparse_infill_density": "15%",
         "wipe_tower_x": ["%g" % (CX + 2)], "wipe_tower_y": ["%g" % (CY - 9)], "prime_tower_width": "20",
         "wipe_tower_wall_type": "rectangle", "prime_tower_brim_width": "0"}
TOWER_MODEL = [(cylinder(CX - 8, CY, 6, 0, 8), 1)]


def _v(prefix, key, *values, **more):
    return [("%s_%s" % (prefix, v), dict(more, **{key: v})) for v in values]


def _all(z, t, tool):
    return True


CASES = {
    # A round part, its outer wall in the second filament, so every layer has a tool change, and a
    # tower beside it.
    "tower": dict(
        objects=TOWER_MODEL, machine=MM_MACHINE, filaments=2, base=TOWER, keep=_all,
        values=[("enable_0", {"enable_prime_tower": "0"}), ("enable_1", {"wipe_tower_wall_type": "rib", "prime_tower_brim_width": "3"})]
        + _v("brim", "prime_tower_brim_width", "0", "4")
        + _v("skip", "prime_tower_skip_points", "0", "1")
        + _v("riblen", "wipe_tower_extra_rib_length", "0", "12", wipe_tower_wall_type="rib")
        + _v("ribw", "wipe_tower_rib_width", "4", "14", wipe_tower_wall_type="rib")
        + _v("fillet", "wipe_tower_fillet_wall", "0", "1", wipe_tower_wall_type="rib")
        + _v("rot", "wipe_tower_rotation_angle", "0", "30", wipe_tower_x=["%g" % (CX + 6)])
        + _v("width", "prime_tower_width", "12", "30")
        + _v("spacing", "wipe_tower_extra_spacing", "100", "200")
        ),
    # A tower beside a tall part: the stabilization cone only reaches past the tower's depth on a tower
    # much taller than it is deep.
    "tower_tall": dict(
        objects=[(cylinder(CX - 6, CY, 4, 0, 24), 1)], machine=MM_MACHINE, filaments=2,
        keep=lambda z, t, tool: t == "Prime tower",
        base=dict(TOWER, prime_tower_width="25", wipe_tower_x=["%g" % (CX + 14)], wipe_tower_y=["%g" % (CY - 4)]),
        values=[("wall_" + v, {"wipe_tower_wall_type": v}) for v in ("rectangle", "cone", "rib")]
        + _v("cone", "wipe_tower_cone_angle", "30", "60", wipe_tower_wall_type="cone")),
    # A part whose bottom and top surfaces are in the second filament: the tower has something to purge
    # only at the bottom and the top, and in between it has its sparse layers.
    "tower_sparse": dict(
        objects=[(cylinder(CX - 8, CY, 6, 0, 10), 1)],
        # A toolhead small enough for "No sparse layers" to reach down beside the part.
        machine=dict(MM_MACHINE, extruder_clearance_radius="2", extruder_clearance_height_to_rod="40",
                     extruder_clearance_height_to_lid="40", nozzle_height="20"),
        filaments=2, keep=_all,
        base=dict(TOWER, outer_wall_filament_id="0", top_surface_filament_id="2", bottom_surface_filament_id="2",
                  wipe_tower_x=["%g" % (CX + 10)]),
        values=_v("nosparse", "wipe_tower_no_sparse_layers", "0", "1")
        + _v("bridging", "wipe_tower_bridging", "4", "16")
        + _v("combine", "wipe_tower_sparse_layers_combination", "0", "1", layer_height="0.1")),
    # Two parts, one in each filament, and where the purge goes.
    "flush": dict(
        objects=[(cylinder(CX - 7, CY + 3, 6.5, 0, 6), 1), (cylinder(CX - 7, CY - 9, 3.5, 0, 6), 2)],
        machine=MM_MACHINE, filaments=2, keep=_all,
        base=dict(TOWER, outer_wall_filament_id="0", flush_into_support="0", wipe_tower_y=["%g" % (CY - 6)],
                  sparse_infill_density="25%"),
        values=[("none", {}), ("infill", {"flush_into_infill": "1"})]),
    # Two blocks side by side, merged into one object, each in its own filament.
    "interlock": dict(
        objects=[(list(box(CX - 8, CY - 5, 0, CX, CY + 5, 6)), 1), (list(box(CX, CY - 5, 0, CX + 8, CY + 5, 6)), 2)],
        machine=MM_MACHINE, filaments=2, assemble=True, keep=_all,
        base={"wall_loops": "2", "sparse_infill_density": "15%", "interlocking_beam": "1", "enable_prime_tower": "0"},
        values=[("beam_0", {"interlocking_beam": "0"}), ("beam_1", {})]
        + _v("width", "interlocking_beam_width", "0.5", "1.6")
        + _v("orient", "interlocking_orientation", "0", "45")
        + _v("layers", "interlocking_beam_layer_count", "1", "4")
        + _v("depth", "interlocking_depth", "1", "4")
        + _v("avoid", "interlocking_boundary_avoidance", "0", "3")),
    # Two blocks stacked, in different filaments: what is printed where they meet.
    "interface_shells": dict(
        objects=[(list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 5)), 1), (list(box(CX - 8, CY - 8, 5, CX + 8, CY + 8, 10)), 2)],
        machine=MM_MACHINE, filaments=2, assemble=True, keep=_all,
        base={"wall_loops": "2", "sparse_infill_density": "15%", "enable_prime_tower": "0"},
        values=_v("shells", "interface_shells", "0", "1")),

    # --- Others ---
    "skirt": dict(
        objects=[(cylinder(CX, CY, 6, 0, 6), 1)], keep=lambda z, t, tool: t in ("Skirt", "Outer wall", "Top surface") or z < 0.25,
        base={"wall_loops": "2", "sparse_infill_density": "15%", "skirt_loops": "1", "skirt_distance": "3"},
        values=_v("loops", "skirt_loops", "1", "3")
        + _v("dist", "skirt_distance", "1", "8")
        + _v("height", "skirt_height", "1", "5")
        + _v("angle", "skirt_start_angle", "-135", "45")
        + _v("minlen", "min_skirt_length", "0", "6")
        + _v("single", "single_loop_draft_shield", "0", "1", draft_shield="enabled", skirt_loops="3")),
    "skirt_type": dict(
        objects=[(cylinder(CX - 7, CY + 4, 4.5, 0, 6), 1), (cylinder(CX + 7, CY - 4, 3.5, 0, 4), 1)],
        keep=lambda z, t, tool: t in ("Skirt", "Outer wall", "Top surface") or z < 0.25,
        base={"wall_loops": "2", "sparse_infill_density": "15%", "skirt_loops": "1", "skirt_distance": "3"},
        values=_v("type", "skirt_type", "combined", "perobject")),
    "combine_brims": dict(
        objects=[(cylinder(CX - 5.5, CY, 4.5, 0, 4), 1), (cylinder(CX + 5.5, CY, 4.5, 0, 4), 1)],
        keep=lambda z, t, tool: t in ("Brim", "Outer wall", "Top surface") or z < 0.25,
        base={"wall_loops": "2", "sparse_infill_density": "15%", "brim_type": "outer_only", "brim_width": "4"},
        values=_v("combine", "combine_brims", "0", "1")),
    "brim": dict(
        objects=[(list(ring(CX, CY, 3.5, 8, 0, 4)), 1)],
        keep=lambda z, t, tool: t in ("Outer wall", "Top surface", "Brim") or z < 0.25,
        base={"wall_loops": "2", "sparse_infill_density": "15%", "brim_type": "outer_only", "brim_width": "4"},
        values=_v("width", "brim_width", "2", "8")
        + _v("gap", "brim_object_gap", "0", "1")
        + _v("efc", "brim_use_efc_outline", "0", "1", elefant_foot_compensation="0.4")),
    # An outline with corners from sharp to blunt (48, 106, 114, 124 and 148 degrees): which of them grow
    # an ear.
    "brim_ears": dict(
        objects=[(list(prism([(CX - 12, CY - 4), (CX + 8, CY - 4), (CX + 10, CY + 3), (CX + 4, CY + 8), (CX - 3, CY + 6)],
                             0, 3)), 1)],
        keep=lambda z, t, tool: z < 0.25,
        base={"wall_loops": "2", "sparse_infill_density": "15%", "brim_type": "brim_ears", "brim_width": "3"},
        values=_v("angle", "brim_ears_max_angle", "60", "130")),
    # A tube with a second, thinner tube inside its wall, as a model drawn in overlapping shells has.
    "slicing_mode": dict(
        objects=[(list(ring(CX, CY, 2.5, 8, 0, 4)) + list(ring(CX, CY, 4.5, 5.5, 0, 4)), 1)],
        keep=_all, base={"wall_loops": "2", "sparse_infill_density": "15%"},
        values=[(v, {"slicing_mode": v}) for v in ("regular", "even_odd", "close_holes")]),
    # Nine small parts on a loose grid; the walls of one layer, in the order they are printed.
    "print_order": dict(
        objects=[(cylinder(CX + gx * 11 + jx, CY + gy * 11 + jy, 2.5, 0, 0.6, 36), 1)
                 for (gx, gy, jx, jy) in ((-1, 1, 2.5, 0), (1, -1, 0, 3), (0, 0, -3, 1.5), (1, 1, 0, -2), (-1, -1, 3, -1),
                                          (0, 1, 3.5, 2), (-1, 0, 0, 3.5), (1, 0, -1.5, -3), (0, -1, -3, 0))],
        keep=lambda z, t, tool: abs(z - 0.4) < 0.01 and t == "Outer wall",
        base={"wall_loops": "1", "sparse_infill_density": "0%", "top_shell_layers": "0", "bottom_shell_layers": "0"},
        values=[(v, {"print_order": v}) for v in ("default", "as_obj_list")]),
    "print_sequence": dict(
        objects=[(cylinder(CX - 6, CY + 4, 4.5, 0, 8), 1), (cylinder(CX + 6, CY - 4, 4.5, 0, 8), 1)],
        keep=lambda z, t, tool: t != "Sparse infill" and t != "Inner wall",
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        values=[("parts", {})]),
    "spiral": dict(
        objects=[(cylinder(CX, CY, 7, 0, 10), 1)], keep=_all,
        base={"wall_loops": "2", "sparse_infill_density": "15%", "bottom_shell_layers": "3"},
        values=_v("spiral", "spiral_mode", "0", "1")),
    # The tube of the noise types' pictures.
    "fuzzy": dict(
        objects=[(cylinder(CX, CY, 7, 0, 8), 1)], keep=lambda z, t, tool: "wall" in t or t == "Top surface",
        base={"wall_loops": "2", "sparse_infill_density": "15%", "fuzzy_skin": "external", "fuzzy_skin_thickness": "0.4"},
        values=_v("pd", "fuzzy_skin_point_distance", "0.2", "1.2")
        + _v("th", "fuzzy_skin_thickness", "0.15", "0.6")
        + _v("scale", "fuzzy_skin_scale", "0.4", "3", fuzzy_skin_noise_type="perlin")
        + _v("oct", "fuzzy_skin_octaves", "1", "8", fuzzy_skin_noise_type="perlin", fuzzy_skin_scale="3")
        + _v("pers", "fuzzy_skin_persistence", "0.15", "0.9", fuzzy_skin_noise_type="perlin", fuzzy_skin_scale="3", fuzzy_skin_octaves="6")
        + _v("rip", "fuzzy_skin_ripples_per_layer", "6", "30", fuzzy_skin_noise_type="ripple")
        + _v("ripoff", "fuzzy_skin_ripple_offset", "0", "50", fuzzy_skin_noise_type="ripple", fuzzy_skin_ripples_per_layer="10")
        + _v("riplayers", "fuzzy_skin_layers_between_ripple_offset", "1", "5", fuzzy_skin_noise_type="ripple", fuzzy_skin_ripples_per_layer="10")
        + _v("first", "fuzzy_skin_first_layer", "0", "1")),
    # One layer of the tube's outer wall, for how each generator mode makes it rough.
    "fuzzy_mode": dict(
        objects=[(cylinder(CX, CY, 7, 0, 3), 1)], flow=True,
        keep=lambda z, t, tool: abs(z - 1.4) < 0.01 and "wall" in t,
        base={"wall_loops": "2", "sparse_infill_density": "15%", "fuzzy_skin": "external", "fuzzy_skin_thickness": "0.3",
              "fuzzy_skin_point_distance": "0.5"},
        values=[(v, {"fuzzy_skin_mode": v}) for v in ("displacement", "extrusion", "combined")]),
}


# --- slicing and reading the G-code --------------------------------------------------------------

_NUM = re.compile(r"([XYZEF])(-?[\d.]+)")
_AREA = math.pi * (1.75 / 2.0) ** 2  # mm² of filament


def parse(gcode, keep, flow=False):
    """The extrusions as layers: [{"z", "paths": [{"t": type, "f": tool, "w": width, "p": [x, y, ...]}]}],
    and with `flow` a "e" list too: each segment's width as the extruded filament fills it."""
    layers, cur, path = [], None, None
    x = y = 0.0
    kind, width, tool, height = "", 0.45, 0, 0.2
    with open(gcode) as f:
        for line in f:
            if line.startswith(";LAYER_CHANGE"):
                cur, path = {"z": None, "paths": []}, None
                layers.append(cur)
            elif line.startswith(";Z:") and cur is not None:
                cur["z"] = float(line[3:])
            elif line.startswith(";HEIGHT:"):
                height = float(line[8:])
            elif line.startswith(";TYPE:"):
                kind, path = line[6:].strip(), None
            elif line.startswith(";WIDTH:"):
                width, path = float(line[7:]), None
            elif re.match(r"T\d+\s*$", line):
                tool, path = int(line[1:]), None
            elif line.startswith("G1 ") or line.startswith("G0 "):
                v = dict((k, float(n)) for k, n in _NUM.findall(line.split(";")[0]))
                nx, ny = v.get("X", x), v.get("Y", y)
                moved = (nx, ny) != (x, y)
                e = v.get("E", 0.0)
                if moved and e > 0.0 and cur is not None:
                    if path is None:
                        path = {"t": kind, "f": tool, "w": round(width, 3), "h": round(height, 3),
                                "p": [round(x - CX, 3), round(y - CY, 3)]}
                        if flow:
                            path["e"] = []
                        cur["paths"].append(path)
                    path["p"] += [round(nx - CX, 3), round(ny - CY, 3)]
                    if flow:
                        length = math.hypot(nx - x, ny - y)
                        path["e"].append(round(e * _AREA / (length * height) + height * (1.0 - math.pi / 4.0), 3))
                elif moved:
                    path = None
                x, y = nx, ny
    out = []
    for layer in layers:
        paths = [p if flow else dict(p, p=_simplified(p["p"])) for p in layer["paths"]
                 if layer["z"] is not None and keep(layer["z"], p["t"], p["f"])]
        if paths:
            out.append({"z": layer["z"], "paths": paths})
    return out


def slice_case(binary, case, value):
    spec = CASES[case]
    name, settings = value
    with tempfile.TemporaryDirectory() as work:
        process = dict(PROCESS, **spec["base"])
        process.update(settings)
        files = {"machine.json": spec.get("machine", MACHINE), "process.json": process}
        count = spec.get("filaments", 1)
        if count > 1:
            files.update({"filament%d.json" % i: _filament(i) for i in range(1, count + 1)})
        for fname, content in files.items():
            with open(os.path.join(work, fname), "w") as f:
                json.dump(content, f)
        stls = []
        for i, (triangles, _) in enumerate(spec["objects"]):
            stls.append(os.path.join(work, "model%d.stl" % i))
            _write_stl(stls[-1], triangles)
        out = os.path.join(work, "out")
        cmd = [binary, "--datadir", os.path.join(work, "datadir"), "--load-settings",
               os.path.join(work, "machine.json") + ";" + os.path.join(work, "process.json")]
        if count > 1:
            cmd += ["--load-filaments", ";".join(os.path.join(work, "filament%d.json" % i) for i in range(1, count + 1)),
                    "--load-filament-ids", ",".join(str(f) for _, f in spec["objects"])]
        if spec.get("assemble"):
            cmd += ["--assemble"]
        cmd += ["--arrange", "0", "--slice", "0", "--outputdir", out] + stls
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        gcode = os.path.join(out, "plate_1.gcode")
        if not os.path.exists(gcode):
            raise RuntimeError("%s=%s did not slice:\n%s" % (case, name, (r.stdout + r.stderr)[-2000:]))
        return parse(gcode, spec["keep"], spec.get("flow", False))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    binary = os.path.abspath(sys.argv[1])
    cases = sys.argv[2:] or list(CASES)
    data = {}
    if os.path.exists(DATA):
        with gzip.open(DATA, "rt") as f:
            data = json.load(f)
    jobs = [(c, v) for c in cases for v in CASES[c]["values"]]
    def run(job):
        try:
            return slice_case(binary, *job)
        except RuntimeError as e:  # reported, and the other values still sliced
            print(e)
            return None

    with ThreadPoolExecutor(max(1, (os.cpu_count() or 4) // 4)) as pool:
        results = list(pool.map(run, jobs))
    for c in cases:
        data[c] = {}
    for (c, v), layers in zip(jobs, results):
        if layers is not None:
            data[c][v[0]] = layers
            print(c, v[0], len(layers), "layers")
    with gzip.open(DATA, "wt") as f:
        json.dump(data, f, separators=(",", ":"))
    print("wrote", DATA, os.path.getsize(DATA) // 1024, "KB")


if __name__ == "__main__":
    main()
