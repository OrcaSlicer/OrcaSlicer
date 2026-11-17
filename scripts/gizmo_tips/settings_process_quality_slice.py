"""The toolpaths behind the pictures of the Process tab's Quality page (settings_process_quality.py).

Like settings_process_slice.py, whose test models and presets it borrows: this slices small models once
per value of a setting with the orca-slicer CLI and keeps the layers the pictures use, in
settings_process_quality_toolpaths.json.gz beside this file. It reads more of the G-code than that
module does, since these settings change more than where the lines go:

- the height of every point, for a scarf seam's ramp and for Z contouring;
- each move's end, for arc fitting, which changes how many moves a line takes rather than its shape;
- travels and wipes, for the seam and travel settings.

    python3 -m gizmo_tips.settings_process_quality_slice <path to orca-slicer> [case ...]   # from scripts/
    python3 scripts/gen_gizmo_tips.py settings_process_quality
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

OUT = "settings/process"  # shares the folder of settings_process_quality.py; draws nothing itself
FRAMES = {}

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "settings_process_quality_toolpaths.json.gz")


# --- more test models ----------------------------------------------------------------------------


def dome(cx, cy, r, segments=64, rings=32):
    """A half sphere standing on its flat side."""
    def at(i, j):
        a = 2 * math.pi * j / segments
        e = 0.5 * math.pi * i / rings
        return (cx + r * math.cos(e) * math.cos(a), cy + r * math.cos(e) * math.sin(a), r * math.sin(e))
    top = (cx, cy, r)
    for j in range(segments):
        k = (j + 1) % segments
        yield (cx, cy, 0.0), at(0, k), at(0, j)
        for i in range(rings - 1):
            yield at(i, j), at(i, k), at(i + 1, k)
            yield at(i, j), at(i + 1, k), at(i + 1, j)
        yield at(rings - 1, j), at(rings - 1, k), top


def plate_with_hole(half, r, z0, z1, segments=96):
    """A square plate with a round hole through its middle."""
    for i in range(segments):
        a0, a1 = 2 * math.pi * i / segments, 2 * math.pi * (i + 1) / segments
        i0 = (CX + r * math.cos(a0), CY + r * math.sin(a0))
        i1 = (CX + r * math.cos(a1), CY + r * math.sin(a1))

        def edge(a):  # where the ray at angle a from the centre leaves the square
            c, s = math.cos(a), math.sin(a)
            t = half / max(abs(c), abs(s))
            return (CX + t * c, CY + t * s)
        o0, o1 = edge(a0), edge(a1)
        for z, up in ((z0, False), (z1, True)):
            quad = [(*o0, z), (*o1, z), (*i1, z), (*i0, z)]
            if not up:
                quad.reverse()
            yield quad[0], quad[1], quad[2]
            yield quad[0], quad[2], quad[3]
        yield (*i1, z0), (*i0, z0), (*i0, z1)
        yield (*i1, z0), (*i0, z1), (*i1, z1)
    yield from _square_sides(half, z0, z1)


def ramp(x0, y0, x1, y1, z0, z1):
    """A block whose top slopes up along x, from z0 at x0 to z1 at x1."""
    v = [(CX + x, CY + y, z) for x, y, z in ((x0, y0, 0), (x1, y0, 0), (x1, y1, 0), (x0, y1, 0),
                                             (x0, y0, z0), (x1, y0, z1), (x1, y1, z1), (x0, y1, z0))]
    for a, b, c, d in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
        yield v[a], v[b], v[c]
        yield v[a], v[c], v[d]


def _square_sides(half, z0, z1):
    c = [(CX - half, CY - half), (CX + half, CY - half), (CX + half, CY + half), (CX - half, CY + half)]
    for i in range(4):
        a, b = c[i], c[(i + 1) % 4]
        yield (*a, z0), (*b, z0), (*b, z1)
        yield (*a, z0), (*b, z1), (*a, z1)


def c_shape(z0, z1):
    """A C opening to the right: a travel between its arms crosses the gap unless it goes round."""
    # Not star-shaped, so it is built from three boxes instead of prism().
    yield from box(CX - 8, CY - 8, z0, CX + 8, CY - 3, z1)
    yield from box(CX - 8, CY + 3, z0, CX + 8, CY + 8, z1)
    yield from box(CX - 8, CY - 3.01, z0, CX - 3, CY + 3.01, z1)


def _rounded_square(half, r, segments=8):
    pts = []
    for cx, cy, a0 in ((half - r, half - r, 0), (r - half, half - r, 90), (r - half, r - half, 180), (half - r, r - half, 270)):
        for i in range(segments + 1):
            a = math.radians(a0 + 90.0 * i / segments)
            pts.append((CX + cx + r * math.cos(a), CY + cy + r * math.sin(a)))
    return pts


DECK = list(box(CX - 10, CY - 6, 0, CX - 6, CY + 6, 6)) + list(box(CX + 6, CY - 6, 0, CX + 10, CY + 6, 6)) \
    + list(box(CX - 10, CY - 6, 5.8, CX + 10, CY + 6, 8))
MUSHROOM = list(ring(CX, CY, 0, 3, 0, 9, 48)) + list(ring(CX, CY, 0, 8, 8, 11, 72))
CLASSIC = {"wall_generator": "classic"}


# --- the cases -----------------------------------------------------------------------------------
#
# As in settings_process_slice.py, plus: machine: settings for the printer; z: keep every point's height;
# ends: keep which points end a G-code move; moves: keep travels ("Travel") and wipes ("Wipe").

CASES = {
    # A half sphere: its outline as steps, fine or coarse.
    "layer_height": dict(
        model=list(dome(CX, CY, 8)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        values=[("0.1", {"layer_height": "0.1", "initial_layer_print_height": "0.1"}),
                ("0.3", {"layer_height": "0.3", "initial_layer_print_height": "0.3"})],
        keep=lambda z, t: t in ("Outer wall", "Top surface", "Overhang wall")),
    # A block's lowest layers.
    "initial_layer_print_height": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 3)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "layer_height": "0.2"},
        key="initial_layer_print_height", values=["0.12", "0.32"],
        keep=lambda z, t: z < 1.7 and "wall" in t),
    # One layer of a rounded block: walls and infill, narrow or wide.
    "line_width": dict(
        model=list(prism(_rounded_square(8, 3), 0, 3)),
        base={"wall_loops": "2", "sparse_infill_density": "20%", "sparse_infill_pattern": "grid"},
        key="line_width", values=["0.3", "0.65"],
        keep=lambda z, t: abs(z - 1.6) < 0.01 or abs(z - 1.8) < 0.01),
    # A cylinder with four walls: where each wall's loop starts.
    "staggered_inner_seams": dict(
        model=list(ring(CX, CY, 0, 7, 0, 3, 90)),
        base=dict(CLASSIC, wall_loops="4", sparse_infill_density="15%", seam_position="back"),
        key="staggered_inner_seams", values=["0", "1"],
        keep=lambda z, t: abs(z - 1.6) < 0.01 and "wall" in t),
    "seam_gap": dict(
        model=list(ring(CX, CY, 0, 7, 0, 3, 90)),
        base=dict(CLASSIC, wall_loops="2", sparse_infill_density="15%", seam_position="back"),
        key="seam_gap", values=["0%", "60%"],
        keep=lambda z, t: abs(z - 1.6) < 0.01 and "wall" in t),
    # A tube: the scarf's ramp on its outer wall and in its hole.
    "seam_slope_type": dict(
        model=list(ring(CX, CY, 3.5, 8, 0, 5, 90)),
        base=dict(CLASSIC, wall_loops="2", sparse_infill_density="15%", seam_position="aligned"),
        key="seam_slope_type", values=["none", "external", "all"],
        keep=lambda z, t: "wall" in t, z=True),
    # Wipes at the end of the outer wall.
    "wipe_on_loops": dict(
        model=list(ring(CX, CY, 0, 7, 0, 3, 90)),
        machine={"wipe": ["1"], "wipe_distance": ["2"]},
        base=dict(CLASSIC, wall_loops="3", sparse_infill_density="15%", seam_position="back"),
        key="wipe_on_loops", values=["0", "1"],
        keep=lambda z, t: 1.3 < z < 1.9, moves=True),
    "wipe_inward": dict(
        model=list(ring(CX, CY, 0, 7, 0, 3, 90)),
        machine={"wipe": ["1"], "wipe_distance": ["2"]},
        base=dict(CLASSIC, wall_loops="3", sparse_infill_density="15%", seam_position="back"),
        key="wipe_inward", values=["0", "1"],
        keep=lambda z, t: 1.3 < z < 1.9, moves=True),
    "wipe_before_external_loop": dict(
        model=list(ring(CX, CY, 0, 7, 0, 3, 90)),
        machine={"wipe": ["1"], "wipe_distance": ["2"]},
        base=dict(CLASSIC, wall_loops="3", sparse_infill_density="15%", seam_position="back",
                  wall_sequence="outer wall/inner wall"),
        key="wipe_before_external_loop", values=["0", "1"],
        keep=lambda z, t: 1.3 < z < 1.9, moves=True),
    # A cylinder's outer wall, simplified finely or coarsely.
    "resolution": dict(
        model=list(ring(CX, CY, 0, 6, 0, 2, 360)),
        base=dict(CLASSIC, wall_loops="2", sparse_infill_density="15%"),
        key="resolution", values=["0.01", "0.3"],
        keep=lambda z, t: abs(z - 1.0) < 0.01 and "wall" in t, ends=True),
    # A cylinder with gyroid infill: how many moves its walls and infill take.
    "enable_arc_fitting": dict(
        model=list(ring(CX, CY, 0, 8, 0, 2, 180)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "sparse_infill_pattern": "gyroid"},
        key="enable_arc_fitting", values=["disabled", "enabled", "sparse_infill"],
        keep=lambda z, t: abs(z - 1.0) < 0.01, ends=True),
    # A square plate with a hole.
    "xy_hole_compensation": dict(
        model=list(plate_with_hole(7, 3, 0, 2)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="xy_hole_compensation", values=["-0.5", "0.5"],
        keep=lambda z, t: abs(z - 1.0) < 0.01),
    "xy_contour_compensation": dict(
        model=list(plate_with_hole(7, 3, 0, 2)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="xy_contour_compensation", values=["-0.5", "0.5"],
        keep=lambda z, t: abs(z - 1.0) < 0.01),
    # A block's lowest layers: the first ones drawn in.
    "elefant_foot_compensation": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 3)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        values=[("off", {"elefant_foot_compensation": "0"}),
                ("on", {"elefant_foot_compensation": "0.5", "elefant_foot_compensation_layers": "1"}),
                ("layers", {"elefant_foot_compensation": "0.5", "elefant_foot_compensation_layers": "5"})],
        keep=lambda z, t: z < 2.1 and "wall" in t),
    "precise_outer_wall": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2)),
        base={"wall_loops": "3", "sparse_infill_density": "15%"},
        key="precise_outer_wall", values=["0", "1"],
        keep=lambda z, t: abs(z - 1.0) < 0.01 and "wall" in t),
    # A small round hole: round, a polygon, a polygon turned on each layer.
    "hole_to_polyhole": dict(
        model=list(plate_with_hole(6, 1.6, 0, 3, 96)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        values=[("off", {"hole_to_polyhole": "0"}),
                ("on", {"hole_to_polyhole": "1", "hole_to_polyhole_twisted": "0"}),
                ("twisted", {"hole_to_polyhole": "1", "hole_to_polyhole_twisted": "1"})],
        keep=lambda z, t: 1.0 < z < 2.25 and "wall" in t),
    # An ironed top, close up.
    "ironing_spacing": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "ironing_type": "top"},
        key="ironing_spacing", values=["0.1", "0.25"],
        keep=lambda z, t: abs(z - 2.0) < 0.01),
    "ironing_inset": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "ironing_type": "top", "ironing_spacing": "0.2"},
        key="ironing_inset", values=["0", "1"],
        keep=lambda z, t: abs(z - 2.0) < 0.01),
    "ironing_angle": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "ironing_type": "top", "ironing_spacing": "0.35"},
        key="ironing_angle", values=["0", "90"],
        keep=lambda z, t: abs(z - 2.0) < 0.01),
    # A gentle ramp: its top surface as steps, or followed within each layer.
    "zaa_enabled": dict(
        model=list(ramp(-8, -6, 8, 6, 0.6, 2.6)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "layer_height": "0.3", "top_shell_layers": "3"},
        key="zaa_enabled", values=["0", "1"],
        keep=lambda z, t: t in ("Outer wall", "Top surface"), z=True),
    # One layer, in the order it is printed.
    "is_infill_first": dict(
        model=list(prism(_rounded_square(7, 2.5), 0, 3)),
        base={"wall_loops": "2", "sparse_infill_density": "25%", "sparse_infill_pattern": "grid"},
        key="is_infill_first", values=["0", "1"],
        keep=lambda z, t: abs(z - 1.4) < 0.01),
    "only_one_wall_first_layer": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2)),
        base={"wall_loops": "3", "sparse_infill_density": "15%"},
        key="only_one_wall_first_layer", values=["0", "1"],
        keep=lambda z, t: z < 0.25),
    "only_one_wall_top": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2)),
        base={"wall_loops": "3", "sparse_infill_density": "15%"},
        key="only_one_wall_top", values=["0", "1"],
        keep=lambda z, t: abs(z - 2.0) < 0.01),
    # A C: travels between its arms, straight across the gap or round through the part.
    "reduce_crossing_wall": dict(
        model=list(c_shape(0, 2)),
        machine={"retraction_minimum_travel": ["0.5"]},
        base={"wall_loops": "2", "sparse_infill_density": "15%", "sparse_infill_pattern": "rectilinear"},
        key="reduce_crossing_wall", values=["0", "1"],
        keep=lambda z, t: abs(z - 1.0) < 0.01, moves=True),
    # The deck over two piers: its first layer bridges the gap.
    "bridge_density": dict(
        model=DECK, base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="bridge_density", values=["50%", "100%"],
        keep=lambda z, t: 5.9 < z < 6.6),
    "enable_extra_bridge_layer": dict(
        model=DECK, base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="enable_extra_bridge_layer", values=["disabled", "external_bridge_only"],
        keep=lambda z, t: 5.9 < z < 6.9),
    # A block with little infill: the first solid layer over it bridges the infill's gaps.
    "internal_bridge_density": dict(
        model=list(box(CX - 10, CY - 10, 0, CX + 10, CY + 10, 6)),
        base={"wall_loops": "2", "sparse_infill_density": "10%", "top_shell_layers": "4"},
        key="internal_bridge_density", values=["50%", "100%"],
        keep=lambda z, t: 4.5 < z < 5.5),
    "thick_internal_bridges": dict(
        model=list(box(CX - 10, CY - 10, 0, CX + 10, CY + 10, 6)),
        base={"wall_loops": "2", "sparse_infill_density": "10%", "top_shell_layers": "4"},
        key="thick_internal_bridges", values=["0", "1"],
        keep=lambda z, t: 4.5 < z < 5.5),
    # The mushroom of settings_process_slice.py: how steep a cone holds up its cap.
    "make_overhang_printable_angle": dict(
        model=MUSHROOM, base={"wall_loops": "2", "sparse_infill_density": "15%", "make_overhang_printable": "1"},
        key="make_overhang_printable_angle", values=["30", "55"],
        keep=lambda z, t: t in ("Outer wall", "Overhang wall", "Top surface", "Bridge")),
}


# --- slicing and reading the G-code --------------------------------------------------------------

_NUM = re.compile(r"([XYZEFIJ])(-?[\d.]+)")


def _arc(x0, y0, x1, y1, i, j, cw):
    """Points along a G2 (cw) or G3 arc, from just after its start to its end."""
    cx, cy = x0 + i, y0 + j
    a0, a1 = math.atan2(y0 - cy, x0 - cx), math.atan2(y1 - cy, x1 - cx)
    if cw:
        while a1 >= a0:
            a1 -= 2 * math.pi
    else:
        while a1 <= a0:
            a1 += 2 * math.pi
    r = math.hypot(x0 - cx, y0 - cy)
    n = max(2, int(abs(a1 - a0) * r / 0.15))
    return [(cx + r * math.cos(a0 + (a1 - a0) * k / n), cy + r * math.sin(a0 + (a1 - a0) * k / n)) for k in range(1, n + 1)]


def parse(gcode, spec):
    """The G-code's moves as layers: [{"z", "h", "paths": [{"t", "w", "p": [x, y, ...]}]}], a path being a run
    of moves of one type. A path also has "z" (each point's height) if the case keeps heights, and "k" (the
    indices of the points that end a G-code move) if it keeps those."""
    keep, want_z, want_ends, want_moves = spec["keep"], spec.get("z"), spec.get("ends"), spec.get("moves")
    layers, cur, path = [], None, None
    x = y = z = 0.0
    kind, width, wiping = "", 0.45, False
    with open(gcode) as f:
        for line in f:
            if line.startswith(";LAYER_CHANGE"):
                cur, path = {"z": None, "h": None, "paths": []}, None
                layers.append(cur)
            elif line.startswith(";Z:") and cur is not None:
                cur["z"] = float(line[3:])
            elif line.startswith(";HEIGHT:") and cur is not None and cur["h"] is None:
                cur["h"] = float(line[8:])  # the layer's; a later one belongs to a single feature
            elif line.startswith(";TYPE:"):
                kind, path = line[6:].strip(), None
            elif line.startswith(";WIDTH:"):
                width, path = float(line[7:]), None
            elif line.startswith(";WIPE_START"):
                wiping, path = True, None
            elif line.startswith(";WIPE_END"):
                wiping, path = False, None
            elif line[:3] in ("G0 ", "G1 ", "G2 ", "G3 "):
                v = dict((k, float(n)) for k, n in _NUM.findall(line.split(";")[0]))
                nx, ny, nz = v.get("X", x), v.get("Y", y), v.get("Z", z)
                if (nx, ny) == (x, y):
                    z = nz
                    continue
                pts = _arc(x, y, nx, ny, v.get("I", 0.0), v.get("J", 0.0), line[:2] == "G2") if line[:2] in ("G2", "G3") \
                    else [(nx, ny)]
                t = kind if v.get("E", 0.0) > 0.0 else ("Wipe" if wiping else "Travel")
                if t in ("Wipe", "Travel") and not want_moves:
                    path = None
                elif cur is not None:
                    if path is None or path["t"] != t:
                        path = {"t": t, "w": round(width, 3), "p": [x, y], "z": [z], "k": [0]}
                        cur["paths"].append(path)
                    for k, (px, py) in enumerate(pts):
                        path["p"] += [px, py]
                        path["z"].append(z + (nz - z) * (k + 1) / len(pts))
                    path["k"].append(len(path["z"]) - 1)
                x, y, z = nx, ny, nz
    out = []
    for layer in layers:
        paths = []
        for p in layer["paths"]:
            if not keep(layer["z"], p["t"]) and p["t"] not in ("Wipe", "Travel"):
                continue
            if p["t"] in ("Wipe", "Travel") and not keep(layer["z"], "Outer wall"):
                continue
            q = {"t": p["t"], "w": p["w"]}
            flat = [round(c - (CX if i % 2 == 0 else CY), 3) for i, c in enumerate(p["p"])]
            if want_z or want_ends:
                q["p"] = [round(c, 3) for c in flat]
                if want_z:
                    q["z"] = [round(c, 3) for c in p["z"]]
                if want_ends:
                    q["k"] = p["k"]
            else:
                q["p"] = _simplified(flat)
            paths.append(q)
        if paths:
            out.append({"z": layer["z"], "h": layer["h"], "paths": paths})
    return out


def slice_case(binary, case, value):
    spec = CASES[case]
    with tempfile.TemporaryDirectory() as work:
        process = dict(PROCESS, **spec["base"])
        process.update(value[1] if isinstance(value, tuple) else {spec["key"]: value})
        files = {"machine.json": dict(MACHINE, **spec.get("machine", {})), "process.json": process}
        for name, content in files.items():
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
        return parse(gcode, spec)


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
        data[c][v[0] if isinstance(v, tuple) else v] = layers
        print(c, v[0] if isinstance(v, tuple) else v, len(layers), "layers")
    with gzip.open(DATA, "wt") as f:
        json.dump(data, f, separators=(",", ":"))
    print("wrote", DATA, os.path.getsize(DATA) // 1024, "KB")


if __name__ == "__main__":
    main()
