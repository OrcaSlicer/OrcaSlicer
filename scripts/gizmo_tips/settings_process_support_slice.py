"""The toolpaths behind the pictures of the Process tab's Support page (settings_process_support.py), sliced
by OrcaSlicer itself, the way settings_process_slice.py does it for the other Process pictures: a few small
test models, sliced once per value of a setting with the orca-slicer CLI, the extrusions read back from the
G-code and kept in settings_process_support_toolpaths.json.gz beside this file.

    python3 -m gizmo_tips.settings_process_support_slice <path to orca-slicer> [case ...]   # from scripts/
    python3 scripts/gen_gizmo_tips.py settings_process_support

Unlike settings_process_slice.py, each path also keeps its layer height ("h"): support layers have heights
of their own, and the side views (sections) of the Z distances draw every layer at its real thickness. A
case may keep only a strip of the model around a plane (`clip`), which is all a section needs.
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

OUT = "settings/process"  # shares the folder of settings_process_support.py; draws nothing itself
FRAMES = {}

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "settings_process_support_toolpaths.json.gz")


# --- test models ---------------------------------------------------------------------------------


def sphere(cx, cy, cz, r, rings=36, segments=72):
    """A UV sphere."""
    def p(i, j):
        th, ph = math.pi * i / rings, 2.0 * math.pi * j / segments
        return (cx + r * math.sin(th) * math.cos(ph), cy + r * math.sin(th) * math.sin(ph), cz - r * math.cos(th))
    for i in range(rings):
        for j in range(segments):
            a, b, c, d = p(i, j), p(i, j + 1), p(i + 1, j + 1), p(i + 1, j)
            if i > 0:
                yield a, b, c
            if i < rings - 1:
                yield a, c, d


# A cap on a stem standing on a wide disc: the support under the cap stands on the disc, so it has a top
# and a bottom against the part.
DISC = list(ring(CX, CY, 0, 9, 0, 2, 72)) + list(ring(CX, CY, 0, 2.5, 0, 7, 48)) + list(ring(CX, CY, 0, 8, 6, 8, 72))
# The same with a disc narrower than the cap: the support stands partly on the disc and partly on the plate.
PLATE = list(ring(CX, CY, 0, 5, 0, 2, 72)) + list(ring(CX, CY, 0, 2, 0, 8, 48)) + list(ring(CX, CY, 0, 9, 7, 9, 72))
# settings_process_slice.py's mushrooms: a low one for the patterns, a tall one for whole supports.
MUSH_LOW = list(ring(CX, CY, 0, 2.5, 0, 9, 48)) + list(ring(CX, CY, 0, 9, 8, 10, 72))
MUSH_TALL = list(ring(CX, CY, 0, 2.5, 0, 16, 48)) + list(ring(CX, CY, 0, 9, 15, 18, 72))
# A ball on a short stem: its underside turns from flat to upright, so a threshold angle is a ring on it.
BALL = list(sphere(CX, CY, 10, 8)) + list(ring(CX, CY, 0, 2.5, 0, 3, 48))
# settings_process_slice.py's bridge: two piers and a deck across them.
BRIDGE = (list(box(CX - 10, CY - 6, 0, CX - 6, CY + 6, 6)) + list(box(CX + 6, CY - 6, 0, CX + 10, CY + 6, 6))
          + list(box(CX - 10, CY - 6, 5.8, CX + 10, CY + 6, 8)))
# A block for a raft.
BLOCK = list(box(CX - 6, CY - 6, 0, CX + 6, CY + 6, 4))
SECTION = 1.2  # a section keeps the paths within this many mm of the plane y = 0

# The process settings every case starts from: settings_process_slice.py's, with supports on.
SUPPORT = {"enable_support": "1", "wall_loops": "2", "sparse_infill_density": "15%"}
NORMAL = dict(SUPPORT, support_type="normal(auto)")
ORGANIC = dict(SUPPORT, support_type="tree(auto)", support_style="organic")
SLIM = dict(SUPPORT, support_type="tree(auto)", support_style="tree_slim")
TIPS = dict(ORGANIC, support_interface_top_layers="0")


def _surface(z, t):
    """What a picture of the model from outside shows: no infill."""
    return t not in ("Sparse infill", "Internal solid infill", "Inner wall", "Internal Bridge")


def _any(z, t):
    return True


def _first(z, t):
    return z < 0.25


# model; base: the process settings all values share; key and values: as in settings_process_slice.py;
# keep(z, type): which extrusions the pictures need; clip: keep only a strip around y = 0, for a section.
CASES = {
    # Side views through the cap and the support under it, standing on the disc.
    "top_z": dict(model=DISC, base=NORMAL, key="support_top_z_distance", values=["0.1", "0.3"], keep=_any, clip=True),
    "bottom_z": dict(model=DISC, base=NORMAL, key="support_bottom_z_distance", values=["0.1", "0.5"], keep=_any, clip=True),
    "top_layers": dict(model=DISC, base=NORMAL, key="support_interface_top_layers", values=["0", "3"], keep=_any, clip=True),
    "bottom_layers": dict(model=DISC, base=NORMAL, key="support_interface_bottom_layers", values=["0", "2"], keep=_any, clip=True),
    "expansion": dict(model=DISC, base=NORMAL, key="support_expansion", values=["0", "1.5"], keep=_any, clip=True),
    "xy_distance": dict(model=DISC, base=NORMAL, key="support_object_xy_distance", values=["0.35", "1.5"], keep=_any, clip=True),
    "raft_contact": dict(model=BLOCK, base=dict(SUPPORT, enable_support="0", raft_layers="3"), key="raft_contact_distance",
                         values=["0.1", "0.3"], keep=_any, clip=True),
    "on_plate": dict(model=PLATE, base=NORMAL, key="support_on_build_plate_only", values=["0", "1"], keep=_any, clip=True),

    # Plans: a layer of the support from above.
    "base_spacing": dict(model=MUSH_LOW, base=NORMAL, key="support_base_pattern_spacing", values=["1", "4"],
                         keep=lambda z, t: 3.3 < z < 4.05),
    "angle": dict(model=MUSH_LOW, base=NORMAL, key="support_angle", values=["0", "45"], keep=lambda z, t: 3.3 < z < 4.05),
    "interface_spacing": dict(model=MUSH_LOW, base=NORMAL, key="support_interface_spacing", values=["0", "1"],
                              keep=lambda z, t: 7.0 < z < 8.0 and t.startswith("Support")),
    "bottom_spacing": dict(model=DISC, base=dict(NORMAL, support_interface_bottom_layers="2"), key="support_bottom_interface_spacing",
                           values=["0", "1"], keep=lambda z, t: 1.9 < z < 2.9),
    "ironing": dict(model=MUSH_LOW, base=NORMAL, key="support_ironing", values=["0", "1"],
                    keep=lambda z, t: 7.0 < z < 8.0 and t.startswith("Support") or t == "Ironing"),
    "ironing_spacing": dict(model=MUSH_LOW, base=dict(NORMAL, support_ironing="1"), key="support_ironing_spacing",
                            values=["0.1", "0.3"], keep=lambda z, t: 7.0 < z < 8.0 and (t.startswith("Support") or t == "Ironing")),
    "first_density": dict(model=BLOCK, base=dict(SUPPORT, enable_support="0", raft_layers="3"), key="raft_first_layer_density",
                          values=["30%", "90%"], keep=_first),
    "first_expansion": dict(model=PLATE, base=NORMAL, key="raft_first_layer_expansion", values=["0", "3"], keep=_first),
    "first_gap": dict(model=PLATE, base=NORMAL, key="support_object_first_layer_gap", values=["0.2", "1.5"], keep=_first),
    "wall_count": dict(model=MUSH_TALL, base=SLIM, key="tree_support_wall_count", values=["1", "2"], keep=lambda z, t: 9.0 < z < 11.0),
    # Organic tips, with no interface over them: an interface takes the place of the tips' own spacing.
    "tip_diameter": dict(model=MUSH_TALL, base=TIPS, key="tree_support_tip_diameter", values=["0.8", "2"], keep=_surface),
    "branch_distance_organic": dict(model=MUSH_TALL, base=TIPS, key="tree_support_branch_distance_organic", values=["1", "4"],
                                    keep=_surface),
    "top_rate": dict(model=MUSH_TALL, base=TIPS, key="tree_support_top_rate", values=["10%", "35%"], keep=_surface),
    "branch_distance": dict(model=MUSH_TALL, base=SLIM, key="tree_support_branch_distance", values=["2", "6"], keep=_surface),

    # Models: the whole part and its support in the plater's view.
    "enable": dict(model=MUSH_TALL, base=dict(SUPPORT, enable_support="0"), key="enable_support", values=["0"], keep=_surface),
    "threshold": dict(model=BALL, base=NORMAL, key="support_threshold_angle", values=["30", "60"], keep=_surface),
    "overlap": dict(model=BALL, base=dict(NORMAL, support_threshold_angle="0"), key="support_threshold_overlap",
                    values=["10%", "90%"], keep=_surface),
    "raft_layers": dict(model=BLOCK, base=dict(SUPPORT, enable_support="0"), key="raft_layers", values=["0", "3"], keep=_surface),
    "raft_expansion": dict(model=BLOCK, base=dict(SUPPORT, enable_support="0", raft_layers="3"), key="raft_expansion",
                           values=["0", "3"], keep=_surface),
    "bridge_no_support": dict(model=BRIDGE, base=NORMAL, key="bridge_no_support", values=["0", "1"], keep=_surface),
    "branch_diameter_organic": dict(model=MUSH_TALL, base=ORGANIC, key="tree_support_branch_diameter_organic", values=["2", "5"],
                                    keep=_surface),
    "diameter_angle": dict(model=MUSH_TALL, base=ORGANIC, key="tree_support_branch_diameter_angle", values=["0", "10"], keep=_surface),
    "branch_angle_organic": dict(model=MUSH_TALL, base=ORGANIC, key="tree_support_branch_angle_organic", values=["20", "55"],
                                 keep=_surface),
    "angle_slow": dict(model=MUSH_TALL, base=ORGANIC, key="tree_support_angle_slow", values=["10", "35"], keep=_surface),
    "branch_diameter": dict(model=MUSH_TALL, base=SLIM, key="tree_support_branch_diameter", values=["2", "6"], keep=_surface),
    "tree_brim": dict(model=MUSH_TALL, base=dict(SLIM, tree_support_auto_brim="0"), key="tree_support_brim_width",
                      values=["0", "5"], keep=_surface),
}


# --- slicing and reading the G-code --------------------------------------------------------------

_NUM = re.compile(r"([XYZEF])(-?[\d.]+)")


def _strip(pts, half):
    """The runs of a polyline (a list of (x, y)) that lie within `half` mm of the plane y = 0."""
    runs, run = [], []
    for i, (x, y) in enumerate(pts):
        inside = abs(y) <= half
        if i > 0:
            (px, py) = pts[i - 1]
            # Where the segment crosses the strip's edges, in order along it.
            ts = sorted(t for t in ((e - py) / (y - py) for e in (-half, half) if (py - e) * (y - e) < 0) if 0 < t < 1)
            for t in ts:
                q = (px + (x - px) * t, py + (y - py) * t)
                if run:
                    run.append(q)
                    runs.append(run)
                    run = []
                else:
                    run = [q]
        if inside:
            run.append((x, y))
    if len(run) > 1:
        runs.append(run)
    return [r for r in runs if len(r) > 1]


def parse(gcode, keep, clip, coarse=False):
    """As settings_process_slice.parse, with each path's layer height in "h". `coarse` keeps fewer points: a
    model view, where a whole part fits in the picture, needs less than a close-up."""
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
                        path = {"t": kind, "w": round(width, 3), "h": round(height, 3), "p": [(x - CX, y - CY)]}
                        cur["paths"].append(path)
                    path["p"].append((nx - CX, ny - CY))
                elif moved:
                    path = None
                x, y = nx, ny
    out = []
    for layer in layers:
        paths = []
        for p in layer["paths"]:
            if not keep(layer["z"], p["t"]):
                continue
            for run in (_strip(p["p"], SECTION) if clip else [p["p"]]):
                flat = [round(c, 3) for q in run for c in q]
                paths.append(dict(p, p=_simplified(flat, 0.08 if coarse else 0.025)))
        if paths:
            out.append({"z": layer["z"], "paths": paths})
    return out


def slice_case(binary, case, value):
    spec = CASES[case]
    with tempfile.TemporaryDirectory() as work:
        process = dict(PROCESS, **spec["base"])
        process[spec["key"]] = value
        for name, content in (("machine.json", MACHINE), ("process.json", process)):
            with open(os.path.join(work, name), "w") as f:
                json.dump(content, f)
        stl = os.path.join(work, "model.stl")
        _write_stl(stl, spec["model"])
        out = os.path.join(work, "out")
        r = subprocess.run([binary, "--datadir", os.path.join(work, "datadir"), "--load-settings",
                            os.path.join(work, "machine.json") + ";" + os.path.join(work, "process.json"),
                            "--arrange", "0", "--slice", "0", "--outputdir", out, stl],
                           capture_output=True, text=True, timeout=900)
        gcode = os.path.join(out, "plate_1.gcode")
        if not os.path.exists(gcode):
            raise RuntimeError("%s=%s did not slice:\n%s" % (case, value, (r.stdout + r.stderr)[-2000:]))
        return parse(gcode, spec["keep"], spec.get("clip", False), spec["keep"] is _surface)


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
    with ThreadPoolExecutor(max(1, (os.cpu_count() or 4) // 4)) as pool:
        results = list(pool.map(lambda j: slice_case(binary, *j), jobs))
    for c in cases:
        data[c] = {}
    for (c, v), layers in zip(jobs, results):
        data[c][v] = layers
        print(c, v, len(layers), "layers")
    with gzip.open(DATA, "wt") as f:
        json.dump(data, f, separators=(",", ":"))
    print("wrote", DATA, os.path.getsize(DATA) // 1024, "KB")


if __name__ == "__main__":
    main()
