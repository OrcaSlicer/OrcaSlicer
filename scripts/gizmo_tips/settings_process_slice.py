"""The toolpaths behind the Process settings' pictures (settings_process.py), sliced by OrcaSlicer itself.

The pictures of the patterns, walls, seams, supports and brims are drawn from real G-code, so that they
show what the slicer prints rather than a guess at it. This slices a few small test models once per
value of each setting with the orca-slicer CLI, reads the extrusions back from the G-code and keeps the
layers the pictures use, in settings_process_toolpaths.json.gz beside this file. The pictures are drawn
from that file, so drawing them needs no slicer.

Run it again after a change to how one of these settings slices, then redraw the set:

    python3 -m gizmo_tips.settings_process_slice <path to orca-slicer> [case ...]   # from scripts/
    python3 scripts/gen_gizmo_tips.py settings_process

The test models are generated here as STL: boxes, prisms and rings, in millimetres, centred on (100, 100)
of a 200 mm bed. The presets are standalone (no "inherits"), so every setting not named in a case keeps
its default from PrintConfig.cpp.
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

OUT = "settings/process"  # shares the folder of settings_process.py; draws nothing itself
FRAMES = {}

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "settings_process_toolpaths.json.gz")
CX = CY = 100.0  # the models' centre on the bed

MACHINE = {"type": "machine", "from": "User", "name": "tips printer",
           "printable_area": ["0x0", "200x0", "200x200", "0x200"], "printable_height": "100",
           "layer_change_gcode": "G92 E0", "use_relative_e_distances": "1"}
PROCESS = {"type": "process", "from": "User", "name": "tips process", "enable_support": "0",
           "skirt_loops": "0", "brim_type": "no_brim"}


# --- test models: lists of triangles, in mm ------------------------------------------------------


def box(x0, y0, z0, x1, y1, z1):
    v = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        yield v[a], v[b], v[c]
        yield v[a], v[c], v[d]


def prism(poly, z0, z1):
    """A star-shaped polygon (counter-clockwise (x, y) points) extruded from z0 to z1."""
    n = len(poly)
    cx = sum(p[0] for p in poly) / n
    cy = sum(p[1] for p in poly) / n
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        yield (cx, cy, z0), (b[0], b[1], z0), (a[0], a[1], z0)
        yield (cx, cy, z1), (a[0], a[1], z1), (b[0], b[1], z1)
        yield (a[0], a[1], z0), (b[0], b[1], z0), (b[0], b[1], z1)
        yield (a[0], a[1], z0), (b[0], b[1], z1), (a[0], a[1], z1)


def ring(cx, cy, r0, r1, z0, z1, segments=72):
    """A tube: outer radius r1, a hole of radius r0 (0 for a solid cylinder)."""
    if r0 <= 0.0:
        yield from prism(circle(cx, cy, r1, segments), z0, z1)
        return
    for i in range(segments):
        a0, a1 = 2 * math.pi * i / segments, 2 * math.pi * (i + 1) / segments
        o0, o1 = (cx + r1 * math.cos(a0), cy + r1 * math.sin(a0)), (cx + r1 * math.cos(a1), cy + r1 * math.sin(a1))
        i0, i1 = (cx + r0 * math.cos(a0), cy + r0 * math.sin(a0)), (cx + r0 * math.cos(a1), cy + r0 * math.sin(a1))
        for z, up in ((z0, False), (z1, True)):
            quad = [(*o0, z), (*o1, z), (*i1, z), (*i0, z)]
            if not up:
                quad.reverse()
            yield quad[0], quad[1], quad[2]
            yield quad[0], quad[2], quad[3]
        yield (*o0, z0), (*o1, z0), (*o1, z1)
        yield (*o0, z0), (*o1, z1), (*o0, z1)
        yield (*i1, z0), (*i0, z0), (*i0, z1)
        yield (*i1, z0), (*i0, z1), (*i1, z1)


def circle(cx, cy, r, segments=72):
    return [(cx + r * math.cos(2 * math.pi * i / segments), cy + r * math.sin(2 * math.pi * i / segments))
            for i in range(segments)]


def _write_stl(path, triangles):
    with open(path, "w") as f:
        f.write("solid m\n")
        for t in triangles:
            f.write("facet normal 0 0 0\nouter loop\n" + "".join("vertex %.4f %.4f %.4f\n" % p for p in t)
                    + "endloop\nendfacet\n")
        f.write("endsolid m\n")


# --- the cases: one per setting, sliced once per value -------------------------------------------
#
# model: triangles; base: the process settings all values share; values: the values to slice, each
# applied as {key: value}, or (name, {settings}) for a value that needs more than its key; keep(z, type):
# which extrusions the pictures need.

SPARSE = ("rectilinear alignedrectilinear zigzag crosszag lockedzag line grid triangles tri-hexagon cubic "
          "adaptivecubic quartercubic supportcubic lightning honeycomb 3dhoneycomb lateral-honeycomb "
          "lateral-lattice crosshatch tpmsd tpmsfk gyroid concentric hilbertcurve archimedeanchords "
          "octagramspiral").split()
SURFACE = "monotonic monotonicline rectilinear alignedrectilinear concentric spiralinset hilbertcurve archimedeanchords octagramspiral".split()

SPARSE_Z = 10.4  # the layer shown: a little under the cube's top shells, where Lightning has branched


def _rounded_square(half, r, segments=8):
    pts = []
    for cx, cy, a0 in ((half - r, half - r, 0), (r - half, half - r, 90), (r - half, r - half, 180), (half - r, r - half, 270)):
        for i in range(segments + 1):
            a = math.radians(a0 + 90.0 * i / segments)
            pts.append((CX + cx + r * math.cos(a), CY + cy + r * math.sin(a)))
    return pts


CASES = {
    # A 30 mm cube; the layer under the top shells and the eight beneath it.
    "sparse_infill_pattern": dict(
        model=list(box(CX - 15, CY - 15, 0, CX + 15, CY + 15, 12)),
        base={"sparse_infill_density": "20%", "wall_loops": "2", "top_shell_layers": "4", "bottom_shell_layers": "3"},
        key="sparse_infill_pattern", values=SPARSE,
        keep=lambda z, t: SPARSE_Z - 1.75 < z < SPARSE_Z + 0.05 and (t == "Sparse infill" or ("wall" in t and z > SPARSE_Z - 0.05))),
    # A block with its back right quarter notched down: an L-shaped top surface, and a square one below it.
    "top_surface_pattern": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2)) + list(box(CX - 8, CY - 8, 0, CX + 8, CY, 4))
        + list(box(CX - 8, CY - 8, 0, CX, CY + 8, 4)),
        base={"sparse_infill_density": "15%", "wall_loops": "2", "top_shell_layers": "3", "bottom_shell_layers": "2"},
        key="top_surface_pattern", values=SURFACE,
        keep=lambda z, t: abs(z - 2.0) < 0.01 or abs(z - 4.0) < 0.01),
    # A round tube of outer walls, sliced whole: where each layer starts.
    "seam_position": dict(
        model=list(ring(CX, CY, 0, 7, 0, 12, 60)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="seam_position", values="nearest aligned aligned_back back random".split(),
        keep=lambda z, t: t == "Outer wall"),
    # One layer of three walls around a rounded square.
    "wall_sequence": dict(
        model=list(prism(_rounded_square(7, 2.5), 0, 3)),
        base={"wall_loops": "3", "sparse_infill_density": "15%"},
        key="wall_sequence", values=["inner wall/outer wall", "outer wall/inner wall", "inner-outer-inner wall"],
        keep=lambda z, t: abs(z - 1.4) < 0.01),
    "wall_direction": dict(
        model=list(prism(_rounded_square(7, 2.5), 0, 3)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="wall_direction", values=["ccw", "cw"],
        keep=lambda z, t: abs(z - 1.4) < 0.01),
    # A wedge narrowing from 5 mm to nothing: where Classic runs out of whole walls, Arachne thins them.
    "wall_generator": dict(
        model=list(prism([(CX - 8, CY - 2.5), (CX + 8, CY - 0.15), (CX + 8, CY + 0.15), (CX - 8, CY + 2.5)], 0, 2)),
        base={"wall_loops": "3", "sparse_infill_density": "15%"},
        key="wall_generator", values=["classic", "arachne"],
        keep=lambda z, t: abs(z - 1.0) < 0.01),
    # A mushroom: a stem carrying a wide round cap. Sliced whole, for the supports under the cap.
    "support_style": dict(
        model=list(ring(CX, CY, 0, 2.5, 0, 16, 48)) + list(ring(CX, CY, 0, 9, 15, 18, 72)),
        base={"enable_support": "1", "wall_loops": "2", "sparse_infill_density": "15%"},
        key="support_style", values=[("grid", {"support_type": "normal(auto)", "support_style": "grid"}),
                                     ("snug", {"support_type": "normal(auto)", "support_style": "snug"}),
                                     ("organic", {"support_type": "tree(auto)", "support_style": "organic"}),
                                     ("tree_slim", {"support_type": "tree(auto)", "support_style": "tree_slim"}),
                                     ("tree_strong", {"support_type": "tree(auto)", "support_style": "tree_strong"}),
                                     ("tree_hybrid", {"support_type": "tree(auto)", "support_style": "tree_hybrid"})],
        keep=lambda z, t: t.startswith("Support") or t in ("Outer wall", "Overhang wall", "Top surface")),
    # The same mushroom, lower: the base's top layers and the interface layers the cap rests on. Normal
    # supports, except for Lightning and Hollow, which only Tree Slim, Strong and Hybrid print (Hybrid
    # here); the others print as Rectilinear there.
    "support_base_pattern": dict(
        model=list(ring(CX, CY, 0, 2.5, 0, 9, 48)) + list(ring(CX, CY, 0, 9, 8, 10, 72)),
        base={"enable_support": "1", "wall_loops": "2", "sparse_infill_density": "15%"},
        key="support_base_pattern",
        values=[(v, {"support_type": "normal(auto)", "support_base_pattern": v}) for v in ("rectilinear", "rectilinear-grid", "honeycomb")]
        + [(v, {"support_type": "tree(auto)", "support_style": "tree_hybrid", "support_base_pattern": v}) for v in ("lightning", "hollow")],
        keep=lambda z, t: 3.3 < z < 4.05),
    "support_interface_pattern": dict(
        model=list(ring(CX, CY, 0, 2.5, 0, 9, 48)) + list(ring(CX, CY, 0, 9, 8, 10, 72)),
        base={"enable_support": "1", "support_type": "normal(auto)", "wall_loops": "2", "sparse_infill_density": "15%"},
        key="support_interface_pattern", values="auto rectilinear concentric spiralinset rectilinear_interlaced grid".split(),
        keep=lambda z, t: 7.0 < z < 8.0 and t.startswith("Support")),
    # A tube, for a brim outside it and one inside its hole.
    "brim_type": dict(
        model=list(ring(CX, CY, 3.5, 8, 0, 4)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "brim_width": "4"},
        key="brim_type", values="auto_brim brim_ears outer_only inner_only outer_and_inner no_brim".split(),
        keep=lambda z, t: t in ("Outer wall", "Top surface", "Brim") or z < 0.25),
    # Two steps: the low one's top surface is not the topmost.
    "ironing_type": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 3)) + list(box(CX - 8, CY - 8, 0, CX, CY + 8, 6)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "top_shell_layers": "4"},
        key="ironing_type", values=["no ironing", "top", "topmost", "solid"],
        keep=lambda z, t: t != "Sparse infill" and t != "Inner wall"),
    # A tube: its contour, its hole and the walls between.
    "fuzzy_skin": dict(
        model=list(ring(CX, CY, 3.5, 8, 0, 8)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="fuzzy_skin", values="external hole all disabled_fuzzy".split(),
        keep=lambda z, t: "wall" in t or t == "Top surface"),
    "fuzzy_skin_noise_type": dict(
        model=list(ring(CX, CY, 0, 7, 0, 8)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "fuzzy_skin": "external", "fuzzy_skin_thickness": "0.4"},
        key="fuzzy_skin_noise_type", values="classic perlin billow ridgedmulti voronoi ripple".split(),
        keep=lambda z, t: "wall" in t or t == "Top surface"),
    # A mushroom with no support: printable turns the cap's underside into a cone.
    "make_overhang_printable": dict(
        model=list(ring(CX, CY, 0, 3, 0, 9, 48)) + list(ring(CX, CY, 0, 8, 8, 11, 72)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="make_overhang_printable", values=["0", "1"],
        keep=lambda z, t: t in ("Outer wall", "Overhang wall", "Top surface", "Bridge")),
    # The same mushroom: the cap's first layers hang over nothing.
    "extra_perimeters_on_overhangs": dict(
        model=list(ring(CX, CY, 0, 3, 0, 9, 48)) + list(ring(CX, CY, 0, 8, 8, 11, 72)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="extra_perimeters_on_overhangs", values=["0", "1"],
        keep=lambda z, t: 7.7 < z < 8.5),
    "ironing_pattern": dict(
        model=list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "ironing_type": "top"},
        key="ironing_pattern", values=["rectilinear", "concentric"],
        keep=lambda z, t: abs(z - 2.0) < 0.01),
    "draft_shield": dict(
        model=list(ring(CX, CY, 0, 5, 0, 8)),
        base={"wall_loops": "2", "sparse_infill_density": "15%", "skirt_loops": "1", "skirt_distance": "3"},
        key="draft_shield", values=["disabled", "enabled"],
        keep=lambda z, t: t in ("Outer wall", "Top surface", "Skirt")),
    # Two piers and a deck across them: the deck's first layer is a bridge.
    "thick_bridges": dict(
        model=list(box(CX - 10, CY - 6, 0, CX - 6, CY + 6, 6)) + list(box(CX + 6, CY - 6, 0, CX + 10, CY + 6, 6))
        + list(box(CX - 10, CY - 6, 5.8, CX + 10, CY + 6, 8)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="thick_bridges", values=["0", "1"],
        keep=lambda z, t: 5.9 < z < 6.6),
    # A counterbore printed upside down: the wide bore at the bottom, the narrow hole above it.
    "counterbore_hole_bridging": dict(
        model=list(ring(CX, CY, 5, 9, 0, 4)) + list(ring(CX, CY, 2, 9, 3.8, 8)),
        base={"wall_loops": "2", "sparse_infill_density": "15%"},
        key="counterbore_hole_bridging", values=["none", "partiallybridge", "sacrificiallayer"],
        keep=lambda z, t: z < 5.05),
}


# --- slicing and reading the G-code --------------------------------------------------------------

_NUM = re.compile(r"([XYZEF])(-?[\d.]+)")


def parse(gcode, keep):
    """The extrusions of the G-code as layers: [{"z", "paths": [{"t": type, "w": width, "p": [x, y, ...]}]}],
    a path being a run of extruding moves with no travel, type or width change between them."""
    layers, cur, path = [], None, None
    x = y = 0.0
    kind, width = "", 0.45
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
            elif line.startswith("G1 ") or line.startswith("G0 "):
                v = dict((k, float(n)) for k, n in _NUM.findall(line.split(";")[0]))
                nx, ny = v.get("X", x), v.get("Y", y)
                moved = (nx, ny) != (x, y)
                if moved and v.get("E", 0.0) > 0.0 and cur is not None:
                    if path is None:
                        path = {"t": kind, "w": round(width, 3), "p": [round(x - CX, 3), round(y - CY, 3)]}
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


def _simplified(flat, tolerance=0.025):
    """A path's points (x, y, x, y, ...) with those that lie within `tolerance` mm of the line through their
    neighbours left out (Douglas-Peucker), rounded to 0.01 mm: finer than a picture shows, and it keeps
    the file small."""
    pts = [(flat[i], flat[i + 1]) for i in range(0, len(flat), 2)]
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        i, j = stack.pop()
        (ax, ay), (bx, by) = pts[i], pts[j]
        ex, ey = bx - ax, by - ay
        length = math.hypot(ex, ey)
        far, at = 0.0, -1
        for k in range(i + 1, j):
            px, py = pts[k][0] - ax, pts[k][1] - ay
            d = abs(px * ey - py * ex) / length if length > 1e-9 else math.hypot(px, py)
            if d > far:
                far, at = d, k
        if far > tolerance:
            keep[at] = True
            stack += [(i, at), (at, j)]
    return [round(c, 2) for (x, y), k in zip(pts, keep) if k for c in (x, y)]


def slice_case(binary, case, value):
    spec = CASES[case]
    with tempfile.TemporaryDirectory() as work:
        process = dict(PROCESS, **spec["base"])
        process.update(value[1] if isinstance(value, tuple) else {spec["key"]: value})
        files = {"machine.json": MACHINE, "process.json": process}
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
        return parse(gcode, spec["keep"])


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
        print(c, v, len(layers), "layers")
    with gzip.open(DATA, "wt") as f:
        json.dump(data, f, separators=(",", ":"))
    print("wrote", DATA, os.path.getsize(DATA) // 1024, "KB")


if __name__ == "__main__":
    main()
