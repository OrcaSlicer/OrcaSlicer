"""The toolpaths behind the pictures of the Process tab's Strength page (settings_process_strength.py),
sliced by OrcaSlicer itself, as settings_process_slice.py does for the earlier Process pictures.

Each case slices one small test model once per value of a setting with the orca-slicer CLI and keeps
the extrusions the pictures use, in settings_process_strength_toolpaths.json.gz beside this file. A case
drawn as a cross-section keeps only the extrusions near its cutting plane (`band`), which is all a
section needs and keeps the file small.

    python3 -m gizmo_tips.settings_process_strength_slice <path to orca-slicer> [case ...]   # from scripts/
    python3 scripts/gen_gizmo_tips.py settings_process_strength

The models are in millimetres, centred on (100, 100) of a 200 mm bed; the presets are standalone, so
every setting not named in a case keeps its default from PrintConfig.cpp.
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

OUT = "settings/process"  # shares the folder of settings_process_strength.py; draws nothing itself
FRAMES = {}

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "settings_process_strength_toolpaths.json.gz")

BASE = {"wall_loops": "2", "sparse_infill_density": "15%"}
SECTION_Y = 0.3  # the cutting plane of the sections, a little off the models' centre line


def _u(*parts):
    return [t for part in parts for t in part]


def _roof(half_x, half_y, z0, z1):
    """A box from z0 with a gable roof rising from z0 at its long sides to z1 along y = 0: two slopes."""
    a, b = (CX - half_x, CY - half_y), (CX + half_x, CY - half_y)
    c, d = (CX + half_x, CY + half_y), (CX - half_x, CY + half_y)
    e, f = (CX - half_x, CY), (CX + half_x, CY)
    v = lambda p, z: (p[0], p[1], z)
    tris = list(box(CX - half_x, CY - half_y, 0, CX + half_x, CY + half_y, z0))
    # The box's own top is replaced by the roof: drop its two top triangles.
    tris = [t for t in tris if not all(abs(p[2] - z0) < 1e-9 for p in t)]
    tris += [(v(a, z0), v(b, z0), v(f, z1)), (v(a, z0), v(f, z1), v(e, z1)),
             (v(c, z0), v(d, z0), v(e, z1)), (v(c, z0), v(e, z1), v(f, z1)),
             (v(a, z0), v(e, z1), v(d, z0)), (v(b, z0), v(c, z0), v(f, z1))]
    return tris


def _star(outer, inner, points=5):
    return [(CX + r * math.cos(math.pi / 2 + i * math.pi / points), CY + r * math.sin(math.pi / 2 + i * math.pi / points))
            for i, r in enumerate([outer, inner] * points)]


def _layers(lo, hi, types=None):
    return lambda z, t: lo - 1e-3 < z < hi + 1e-3 and (types is None or t in types or any(s in t for s in types))


ALL = lambda z, t: True
SPARSE_LAYER = _layers(4.8, 5.0)  # the layer the plans of the sparse infill show, and the one under it

CUBE = list(box(CX - 10, CY - 10, 0, CX + 10, CY + 10, 8))

CASES = {
    # --- walls ---------------------------------------------------------------------------------
    "wall_loops": dict(model=list(box(CX - 7, CY - 7, 0, CX + 7, CY + 7, 2)), base=BASE,
                       key="wall_loops", values=["1", "3", "5"], keep=_layers(1.0, 1.0)),
    "alternate_extra_wall": dict(model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 4)),
                                 base=dict(BASE, ensure_vertical_shell_thickness="none"),
                                 key="alternate_extra_wall", values=["0", "1"], keep=ALL, band=True),
    # Two fins, 0.35 and 0.6 mm thick: too thin for two lines of a 0.4 mm nozzle. Left alone, the thinner
    # one is dropped and the other printed as a squeezed loop.
    "detect_thin_wall": dict(model=_u(box(CX - 6, CY - 2.175, 0, CX + 6, CY - 1.825, 2), box(CX - 6, CY + 1.7, 0, CX + 6, CY + 2.3, 2)),
                             base=dict(BASE, wall_generator="classic"),
                             key="detect_thin_wall", values=["0", "1"], keep=_layers(1.0, 1.0)),
    # --- top and bottom shells -----------------------------------------------------------------
    "shells": dict(model=list(box(CX - 5, CY - 5, 0, CX + 5, CY + 5, 7)),
                   base=dict(BASE, top_shell_thickness="0", bottom_shell_thickness="0"),
                   values=[("2", {"top_shell_layers": "2", "bottom_shell_layers": "2"}),
                           ("5", {"top_shell_layers": "5", "bottom_shell_layers": "5"})], keep=ALL, band=True),
    "surface_density": dict(model=list(box(CX - 7, CY - 7, 0, CX + 7, CY + 7, 2)), base=BASE,
                            values=[(v, {"top_surface_density": v + "%", "bottom_surface_density": v + "%"}) for v in ("100", "50", "25")],
                            keep=lambda z, t: z < 0.25 or z > 1.95),
    "fill_order": dict(model=list(box(CX - 7, CY - 7, 0, CX + 7, CY + 7, 2)), base=dict(BASE, top_surface_pattern="concentric"),
                       key="top_surface_fill_order", values=["default", "outward", "inward"], keep=_layers(2.0, 2.0)),
    # Every direction the page sets, at 0 and at 30 degrees.
    "direction": dict(model=list(box(CX - 7, CY - 7, 0, CX + 7, CY + 7, 3)), base=dict(BASE, sparse_infill_pattern="rectilinear"),
                      values=[(v, {k: v for k in ("infill_direction", "solid_infill_direction", "top_layer_direction",
                                                  "bottom_layer_direction")}) for v in ("0", "30")],
                      keep=lambda z, t: z < 0.25 or z > 2.95 or abs(z - 1.6) < 0.01 or abs(z - 2.6) < 0.01),
    # A plate with a peg standing in its left half and a rib across its right half: the peg leaves a hole
    # in the plate's top surface, the rib cuts it in two.
    "top_surface_expansion": dict(
        model=_u(box(CX - 11, CY - 8, 0, CX + 11, CY + 8, 2), ring(CX - 5, CY, 0, 2, 2, 4, 32), box(CX + 3, CY - 8, 2, CX + 7, CY + 8, 4)),
        base=dict(BASE, top_surface_pattern="monotonic"),
        values=[("0", {"top_surface_expansion": "0"})]
        + [(v, {"top_surface_expansion": "2", "top_surface_expansion_direction": v}) for v in ("inward_and_outward", "inward", "outward")]
        + [("margin", {"top_surface_expansion": "2", "top_surface_expansion_margin": "1.5"})],
        keep=lambda z, t: abs(z - 2.0) < 1e-3 or (abs(z - 2.2) < 1e-3 and t == "Outer wall")),
    # A U, its two arms standing on a bridge between them, and a block beside it: three top surfaces on
    # two bodies.
    "center_of_surface_pattern": dict(
        model=_u(box(CX - 11, CY - 4, 0, CX - 1, CY + 4, 1), box(CX - 11, CY - 4, 1, CX - 7, CY + 4, 3),
                 box(CX - 5, CY - 4, 1, CX - 1, CY + 4, 3), box(CX + 2, CY - 4, 0, CX + 10, CY + 4, 3)),
        base=dict(BASE, top_surface_pattern="archimedeanchords"),
        key="center_of_surface_pattern", values=["each_surface", "each_model", "each_assembly"], keep=_layers(3.0, 3.0)),
    "top_bottom_infill_wall_overlap": dict(model=list(box(CX - 7, CY - 7, 0, CX + 7, CY + 7, 2)), base=dict(BASE, top_surface_pattern="rectilinear"),
                                           key="top_bottom_infill_wall_overlap", values=["0", "50"], keep=_layers(2.0, 2.0)),
    # --- sparse infill -------------------------------------------------------------------------
    "sparse_infill_density": dict(model=CUBE, base=BASE, key="sparse_infill_density", values=["10%", "25%", "50%"], keep=SPARSE_LAYER),
    "fill_multiline": dict(model=CUBE, base=dict(BASE, sparse_infill_pattern="grid"), key="fill_multiline", values=["1", "2", "3"],
                           keep=SPARSE_LAYER),
    "sparse_infill_smooth_factor": dict(model=CUBE, base=dict(BASE, sparse_infill_pattern="honeycomb"), key="sparse_infill_smooth_factor", values=["0%", "100%"], keep=SPARSE_LAYER),
    "infill_direction_template": dict(model=CUBE, base=dict(BASE, sparse_infill_pattern="rectilinear"),
                                      values=[("none", {}), ("template", {"sparse_infill_rotate_template": "0,30,60,90,120,150"})],
                                      keep=_layers(3.6, 5.0)),
    "infill_shift_step": dict(model=CUBE, base=dict(BASE, sparse_infill_pattern="crosszag"), key="infill_shift_step",
                              values=["0", "1.5"], keep=_layers(4.0, 5.0, ("Sparse infill", "wall"))),
    "lockedzag": dict(model=CUBE, base=dict(BASE, sparse_infill_pattern="lockedzag"),
                      values=[("default", {}), ("skin_density", {"skin_infill_density": "50%"}),
                              ("skeleton_density", {"skeleton_infill_density": "50%"}), ("skin_depth", {"skin_infill_depth": "5"}),
                              ("skin_width", {"skin_infill_line_width": "160%"}),
                              ("skeleton_width", {"skeleton_infill_line_width": "160%"})],
                      keep=SPARSE_LAYER),
    # Adaptive density, on a cube: the layer through its middle.
    "adaptive_infill_mode": dict(model=list(box(CX - 12, CY - 12, 0, CX + 12, CY + 12, 24)),
                                 base=dict(BASE, sparse_infill_pattern="gyroid", sparse_infill_density="30%", adaptive_infill_interior_density="5%"),
                                 key="adaptive_infill_mode",
                                 values="disabled distance_warp smooth_blend stepped_shells lobes normal_z normal_y normal_x".split(),
                                 keep=_layers(12.0, 12.0)),
    "adaptive_infill_interior_density": dict(model=list(box(CX - 12, CY - 12, 0, CX + 12, CY + 12, 24)),
                                             base=dict(BASE, sparse_infill_pattern="gyroid", sparse_infill_density="30%",
                                                       adaptive_infill_mode="distance_warp"),
                                             key="adaptive_infill_interior_density", values=["5%", "20%"], keep=_layers(12.0, 12.0)),
    "adaptive_infill_gradient": dict(model=list(box(CX - 12, CY - 12, 0, CX + 12, CY + 12, 24)),
                                     base=dict(BASE, sparse_infill_pattern="gyroid", sparse_infill_density="30%",
                                               adaptive_infill_interior_density="5%", adaptive_infill_mode="distance_warp"),
                                     key="adaptive_infill_gradient", values=["linear", "quadratic", "exponential"], keep=_layers(12.0, 12.0)),
    "gyroid_optimized": dict(model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 12)),
                             base=dict(BASE, sparse_infill_pattern="gyroid", sparse_infill_density="10%"),
                             key="gyroid_optimized", values=["0", "1"], keep=ALL, band=True),
    "lateral_lattice": dict(model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 10)),
                            base=dict(BASE, sparse_infill_pattern="lateral-lattice", sparse_infill_density="20%"),
                            values=[("default", {}), ("angle_1", {"lateral_lattice_angle_1": "-15"}),
                                    ("angle_2", {"lateral_lattice_angle_2": "15"})], keep=ALL, band=True),
    "infill_complete_top": dict(model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 10)),
                                base=dict(BASE, sparse_infill_pattern="3dhoneycomb", sparse_infill_density="20%"),
                                key="infill_complete_top", values=["0", "1"], keep=ALL, band=True),
    "infill_overhang_angle": dict(model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 10)),
                                  base=dict(BASE, sparse_infill_pattern="lateral-honeycomb", sparse_infill_density="20%"),
                                  key="infill_overhang_angle", values=["60", "30"], keep=ALL, band=True),
    # Lightning under a cube's top: its trees, sliced whole.
    "lightning": dict(model=list(box(CX - 10, CY - 10, 0, CX + 10, CY + 10, 14)),
                      base=dict(BASE, sparse_infill_pattern="lightning"),
                      values=[("default", {}), ("overhang", {"lightning_overhang_angle": "70"}),
                              ("prune", {"lightning_prune_angle": "75"}), ("straightening", {"lightning_straightening_angle": "10"})],
                      keep=lambda z, t: t == "Sparse infill" or (t in ("Outer wall",) and z < 0.25)),
    "infill_anchor": dict(model=CUBE, base=dict(BASE, sparse_infill_pattern="rectilinear"),
                          values=[("none", {"infill_anchor_max": "0"}), ("default", {})],
                          keep=SPARSE_LAYER),
    "infill_wall_overlap": dict(model=CUBE, base=dict(BASE, sparse_infill_pattern="rectilinear", infill_anchor_max="0"),
                                key="infill_wall_overlap", values=["0", "60"], keep=SPARSE_LAYER),
    # Two blocks in one object, side by side.
    "separated_infills": dict(model=_u(box(CX - 11, CY - 6, 0, CX - 1, CY + 6, 8), box(CX + 1, CY - 6, 0, CX + 11, CY + 6, 8)),
                              base=dict(BASE, sparse_infill_pattern="archimedeanchords", sparse_infill_density="20%"),
                              key="separated_infills", values=["0", "1"], keep=SPARSE_LAYER),
    # --- solid infill and gaps ------------------------------------------------------------------
    # A five-pointed star, solid all through: lines of solid infill cannot reach into its points, and gap
    # fill finishes them.
    "gap_fill": dict(model=list(prism(_star(9.0, 3.6), 0, 1.2)), base=dict(BASE, top_shell_layers="3", bottom_shell_layers="3"),
                     values=[(v, {"gap_fill_target": v}) for v in ("everywhere", "topbottom", "nowhere")]
                     + [("filtered", {"gap_fill_target": "everywhere", "filter_out_gap_fill": "2"})],
                     keep=ALL),
    "detect_narrow_internal_solid_infill": dict(
        model=_u(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2.4), box(CX - 8, CY - 8, 2.4, CX + 8, CY - 5.4, 5)),
        base=dict(BASE, top_shell_layers="5", bottom_shell_layers="3", internal_solid_infill_pattern="monotonic"),
        key="detect_narrow_internal_solid_infill", values=["0", "1"], keep=_layers(3.6, 3.6)),
    # --- advanced ------------------------------------------------------------------------------
    "align_infill_direction_to_model": dict(model=list(box(CX - 8, CY - 4, 0, CX + 8, CY + 4, 2)),
                                            base=dict(BASE, top_surface_pattern="monotonic"), rotate="30",
                                            key="align_infill_direction_to_model", values=["0", "1"],
                                            keep=lambda z, t: t == "Top surface" or "wall" in t),
    "extra_solid_infills": dict(model=list(box(CX - 6, CY - 6, 0, CX + 6, CY + 6, 6)),
                                base=dict(BASE, top_shell_thickness="0", bottom_shell_thickness="0", top_shell_layers="3", bottom_shell_layers="3"),
                                key="extra_solid_infills", values=["", "8#2"], keep=ALL, band=True),
    # Two piers and a deck across them, as for thick bridges, the gap running along x: the bridge's own
    # direction is 90 degrees, so 30 degrees set outright and added to it differ.
    "bridge_angle": dict(model=_u(box(CX - 6, CY - 10, 0, CX + 6, CY - 6, 6), box(CX - 6, CY + 6, 0, CX + 6, CY + 10, 6),
                                  box(CX - 6, CY - 10, 6, CX + 6, CY + 10, 8)),
                         base=BASE, values=[("0", {"bridge_angle": "0"}), ("30", {"bridge_angle": "30"}),
                                            ("relative", {"bridge_angle": "30", "relative_bridge_angle": "1"})],
                         keep=_layers(6.2, 6.2)),
    # The first solid layer over a cube's sparse infill.
    "internal_bridge_angle": dict(model=CUBE, base=dict(BASE, top_shell_layers="4", top_shell_thickness="0"),
                                  values=[("0", {"internal_bridge_angle": "0"}), ("45", {"internal_bridge_angle": "45"})],
                                  keep=lambda z, t: 7.35 < z < 7.45 or (7.15 < z < 7.25 and t == "Sparse infill")),
    # A big block and a thin post: the post's infill area is under the threshold.
    "minimum_sparse_infill_area": dict(model=_u(box(CX - 10, CY - 6, 0, CX + 2, CY + 6, 6), box(CX + 5, CY - 2.5, 0, CX + 10, CY + 2.5, 6)),
                                       base=BASE, key="minimum_sparse_infill_area", values=["0", "15"], keep=_layers(3.0, 3.0)),
    "infill_combination": dict(model=list(box(CX - 6, CY - 6, 0, CX + 6, CY + 6, 5)),
                               base=dict(BASE, layer_height="0.1", initial_layer_print_height="0.2", top_shell_thickness="0",
                                         bottom_shell_thickness="0", top_shell_layers="4", bottom_shell_layers="4"),
                               values=[("off", {"infill_combination": "0"}), ("100%", {"infill_combination": "1"}),
                                       ("50%", {"infill_combination": "1", "infill_combination_max_layer_height": "50%"})],
                               keep=ALL, band=True),
    # A gable roof: two slopes, under which the slicer may add solid infill.
    "ensure_vertical_shell_thickness": dict(model=_roof(8, 8, 1, 7), base=dict(BASE, top_shell_thickness="0", top_shell_layers="4"),
                                            key="ensure_vertical_shell_thickness",
                                            values=["none", "ensure_critical_only", "ensure_moderate", "ensure_all"], keep=ALL, band="x"),
}

BAND = 0.8  # mm either side of the cutting plane


# --- slicing and reading the G-code --------------------------------------------------------------

_NUM = re.compile(r"([XYZEF])(-?[\d.]+)")


def parse(gcode, keep):
    """As settings_process_slice.parse(), with each path's layer height in "h"."""
    layers, cur, path = [], None, None
    x = y = 0.0
    kind, width, height = "", 0.45, 0.2
    with open(gcode) as f:
        for line in f:
            if line.startswith(";LAYER_CHANGE"):
                cur, path = {"z": None, "paths": []}, None
                layers.append(cur)
            elif line.startswith(";Z:") and cur is not None:
                cur["z"] = float(line[3:])
            elif line.startswith(";TYPE:"):
                kind, path = line[6:].strip(), None
            elif line.startswith(";WIDTH:"):
                width, path = float(line[7:]), None
            elif line.startswith(";HEIGHT:"):
                height, path = float(line[8:]), None
            elif line.startswith("G1 ") or line.startswith("G0 ") or line.startswith("G2 ") or line.startswith("G3 "):
                v = dict((k, float(n)) for k, n in _NUM.findall(line.split(";")[0]))
                nx, ny = v.get("X", x), v.get("Y", y)
                moved = (nx, ny) != (x, y)
                if moved and v.get("E", 0.0) > 0.0 and cur is not None:
                    if path is None:
                        path = {"t": kind, "w": round(width, 3), "h": round(height, 3), "p": [round(x - CX, 3), round(y - CY, 3)]}
                        cur["paths"].append(path)
                    path["p"] += [round(nx - CX, 3), round(ny - CY, 3)]
                elif moved:
                    path = None
                x, y = nx, ny
    out = []
    for layer in layers:
        paths = [dict(p, p=_simplified(p["p"])) for p in layer["paths"] if keep(layer["z"], p["t"])]
        if paths:
            out.append({"z": layer["z"], "paths": paths})
    return out


def _band(layers, axis):
    """Only the runs of each path that come within BAND of the cutting plane (y = SECTION_Y, or x = it)."""
    a = 0 if axis == "x" else 1
    out = []
    for layer in layers:
        paths = []
        for p in layer["paths"]:
            pts = [(p["p"][i], p["p"][i + 1]) for i in range(0, len(p["p"]), 2)]
            run = []
            for i in range(len(pts) - 1):
                c0, c1 = pts[i][a] - SECTION_Y, pts[i + 1][a] - SECTION_Y
                near = min(c0, c1) <= BAND and max(c0, c1) >= -BAND
                if near:
                    if not run:
                        run = [pts[i]]
                    run.append(pts[i + 1])
                elif run:
                    paths.append(dict(p, p=[c for q in run for c in q]))
                    run = []
            if run:
                paths.append(dict(p, p=[c for q in run for c in q]))
        if paths:
            out.append({"z": layer["z"], "paths": paths})
    return out


def slice_case(binary, case, value):
    spec = CASES[case]
    with tempfile.TemporaryDirectory() as work:
        process = dict(PROCESS, **spec["base"])
        process.update(value[1] if isinstance(value, tuple) else {spec["key"]: value})
        for name, content in (("machine.json", MACHINE), ("process.json", process)):
            with open(os.path.join(work, name), "w") as f:
                json.dump(content, f)
        stl = os.path.join(work, "model.stl")
        _write_stl(stl, spec["model"])
        out = os.path.join(work, "out")
        # The CLI refuses to rotate an object it is told not to arrange; arranged, it lands back near the
        # bed's centre, and the pictures centre on what was printed.
        place = ["--rotate", spec["rotate"]] if "rotate" in spec else ["--arrange", "0"]
        r = subprocess.run([binary, "--datadir", os.path.join(work, "datadir"), "--load-settings",
                            os.path.join(work, "machine.json") + ";" + os.path.join(work, "process.json")]
                           + place + ["--slice", "0", "--outputdir", out, stl],
                           capture_output=True, text=True, timeout=900)
        gcode = os.path.join(out, "plate_1.gcode")
        if not os.path.exists(gcode):
            raise RuntimeError("%s=%s did not slice:\n%s" % (case, value, (r.stdout + r.stderr)[-2000:]))
        layers = parse(gcode, spec["keep"])
        if spec.get("band"):
            layers = _band(layers, spec["band"])
        return layers


def load():
    if not os.path.exists(DATA):
        return {}
    with gzip.open(DATA, "rt") as f:
        return json.load(f)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    binary = os.path.abspath(sys.argv[1])
    cases = sys.argv[2:] or list(CASES)
    data = load()
    jobs = [(c, v) for c in cases for v in CASES[c]["values"]]
    with ThreadPoolExecutor(max(1, (os.cpu_count() or 4) // 4)) as pool:
        results = list(pool.map(lambda j: slice_case(binary, *j), jobs))
    for c in cases:
        data[c] = {}
    for (c, v), layers in zip(jobs, results):
        data[c][v[0] if isinstance(v, tuple) else v] = layers
        print(c, v[0] if isinstance(v, tuple) else v, len(layers), "layers")
    with gzip.open(DATA, "wt") as f:
        json.dump(data, f, separators=(",", ":"))
    print("wrote", DATA, os.path.getsize(DATA) // 1024, "KB")


if __name__ == "__main__":
    main()
