"""The toolpaths behind the pictures of the Process settings that settings_process_extra.py draws, sliced by
OrcaSlicer itself.

Works like settings_process_others_slice.py (several objects, a filament each, a second filament), whose
presets and test models it reuses, and keeps a little more of the G-code where a setting needs it: the
travel moves and where the nozzle retracts ("moves"), the outlines written for EXCLUDE_OBJECT ("exclude"),
and the extrusions printed before the first layer starts, such as the priming lines ("start", kept as a
layer of its own at z = -1). The data is in settings_process_extra_toolpaths.json.gz beside this file.

    python3 -m gizmo_tips.settings_process_extra_slice <path to orca-slicer> [case ...]   # from scripts/
    python3 scripts/gen_gizmo_tips.py settings_process_extra
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

from .settings_process_others_slice import MM_MACHINE, TOWER, _filament, cylinder
from .settings_process_slice import CX, CY, MACHINE, PROCESS, _simplified, _write_stl, box, prism, ring
from .settings_process_support_slice import MUSH_LOW, MUSH_TALL

OUT = "settings/process"  # shares the folder of settings_process_extra.py; draws nothing itself
FRAMES = {}

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "settings_process_extra_toolpaths.json.gz")


def _v(prefix, key, *values, **more):
    return [("%s_%s" % (prefix, v), dict(more, **{key: v})) for v in values]


def _all(z, t, tool):
    return True


def _solid(z, t, tool):
    """What a picture of the model from outside shows: no infill."""
    return t not in ("Sparse infill", "Internal solid infill", "Inner wall", "Internal Bridge")


def wedge(x0, x1, y0, y1, z0, z_low, z_high):
    """A block whose top slopes from z_low at x0 up to z_high at x1."""
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z_low), (x1, y0, z_high), (x1, y1, z_high),
         (x0, y1, z_low)]
    for a, b, c, d in ((0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (2, 3, 7, 6), (0, 4, 7, 3), (1, 2, 6, 5)):
        yield v[a], v[b], v[c]
        yield v[a], v[c], v[d]


def cone(cx, cy, r0, r1, z0, z1, segments=72):
    """A frustum from radius r0 at z0 to r1 at z1, closed at both ends."""
    for i in range(segments):
        a0, a1 = 2 * math.pi * i / segments, 2 * math.pi * (i + 1) / segments
        p = lambda r, a, z: (cx + r * math.cos(a), cy + r * math.sin(a), z)
        yield (cx, cy, z0), p(r0, a1, z0), p(r0, a0, z0)
        yield (cx, cy, z1), p(r1, a0, z1), p(r1, a1, z1)
        yield p(r0, a0, z0), p(r0, a1, z0), p(r1, a1, z1)
        yield p(r0, a0, z0), p(r1, a1, z1), p(r1, a0, z1)


def _block_with_slit(gap):
    """A 16 mm block with a slit `gap` mm wide cut 8 mm into it from the front."""
    h = gap / 2.0
    return (list(box(CX - 8, CY - 8, 0, CX - h, CY, 2)) + list(box(CX + h, CY - 8, 0, CX + 8, CY, 2))
            + list(box(CX - 8, CY, 0, CX + 8, CY + 8, 2)))


def _blind_hole():
    """An 18 mm disc with a round hole 6 mm across, 4 mm deep, open at the bottom: a recess in its base."""
    out = list(ring(CX, CY, 3.0, 9.0, 0, 4, 72))
    out += list(ring(CX, CY, 0, 9.0, 4, 6, 72))
    return out


def _wing(mirror):
    """A lopsided outline, and its mirror image across a line along Y."""
    pts = [(-7, -6), (5, -6), (7, 1), (1, 6), (-7, 6)]
    if mirror:
        pts = [(-x, y) for x, y in reversed(pts)]
    return pts


# A stem and a cap that flares out over it at 60 degrees from the vertical: each layer of the cap overhangs
# the one under it by more than the default reversal threshold, half a line width.
FLARE = list(ring(CX, CY, 0, 4, 0, 3, 72)) + list(cone(CX, CY, 4, 11, 3, 7)) + list(ring(CX, CY, 0, 11, 7, 8, 72))
# A step: a 4 mm wide shelf in front of the upper block, and a narrow one (2.5 mm) beside it.
SHELVES = list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 3)) + list(box(CX - 8, CY - 4, 0, CX + 5.5, CY + 8, 6))
# A flat cone (an ordinary overhang), and beside it a sharp tail: a point hanging in the air under a
# ledge.
TAIL = (list(cone(CX, CY, 3, 10, 0, 3.5)) + list(ring(CX, CY, 0, 10, 3.5, 6, 72)) + list(cone(CX - 3, CY + 13, 0.3, 3, 3, 6))
        + list(box(CX - 3, CY + 7, 4, CX + 3, CY + 13, 6)))

BASE = {"wall_loops": "2", "sparse_infill_density": "15%"}
SUPPORT = dict(BASE, enable_support="1", support_type="normal(auto)")
SLIM = dict(BASE, enable_support="1", support_type="tree(auto)", support_style="tree_slim")
TREE = dict(BASE, enable_support="1", support_type="tree(auto)")


CASES = {
    # --- Quality ---------------------------------------------------------------------------------
    # A block with a gently sloping top over sparse infill: where the top's solid layers start, over
    # nothing much.
    "internal_bridges": dict(
        objects=[(list(wedge(CX - 10, CX + 10, CY - 8, CY + 8, 0, 3, 6)), 1)],
        base=dict(BASE, sparse_infill_density="10%", top_shell_layers="4"),
        keep=lambda z, t, tool: z > 2.5,
        values=[(v, {"dont_filter_internal_bridges": v}) for v in ("disabled", "limited", "nofilter")]),
    "ef_density": dict(
        objects=[(list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 3)), 1)],
        base=dict(BASE, bottom_shell_layers="5", elefant_foot_compensation="0.3", elefant_foot_compensation_layers="3"),
        keep=lambda z, t, tool: z < 1.05,
        values=_v("density", "elefant_foot_layers_density", "100%", "50%")),
    # Two steps whose tops are an odd number of layers apart, so their top surfaces run across each other.
    "ironing_fixed": dict(
        objects=[(list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 3)) + list(box(CX - 8, CY - 8, 0, CX, CY + 8, 5.8)), 1)],
        base=dict(BASE, ironing_type="top", ironing_spacing="0.4", ironing_angle="0", top_shell_layers="3"),
        keep=lambda z, t, tool: abs(z - 3.0) < 0.01 or abs(z - 5.8) < 0.01,
        values=_v("fixed", "ironing_angle_fixed", "0", "1")),
    "hole_size": dict(
        objects=[(_blind_hole(), 1)],
        base=dict(BASE, make_overhang_printable="1"),
        keep=_all, values=_v("hole", "make_overhang_printable_hole_size", "0", "50")),
    "one_wall_threshold": dict(
        objects=[(SHELVES, 1)],
        base=dict(BASE, wall_loops="3", only_one_wall_top="1"),
        keep=lambda z, t, tool: 2.5 < z < 3.05,
        values=_v("thr", "min_width_top_surface", "0", "5")),
    "reverse": dict(
        objects=[(FLARE, 1)],
        base=dict(BASE, wall_loops="2"),
        keep=lambda z, t, tool: "wall" in t,
        values=[("off", {}), ("on", {"overhang_reverse": "1"}),
                ("internal", {"overhang_reverse": "1", "overhang_reverse_internal_only": "1"}),
                ("thr0", {"overhang_reverse": "1", "overhang_reverse_threshold": "0"})]),
    "precise_wall": dict(
        objects=[(list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2)), 1)],
        base=dict(BASE, wall_loops="3"),
        keep=lambda z, t, tool: abs(z - 1.0) < 0.01,
        values=_v("precise", "precise_outer_wall", "0", "1")),
    "precise_z": dict(
        objects=[(list(box(CX - 6, CY - 6, 0, CX + 6, CY + 6, 3.1)), 1)],
        base=dict(BASE, layer_height="0.2"),
        keep=lambda z, t, tool: t == "Outer wall",
        values=_v("z", "precise_z_height", "0", "1")),
    "closing": dict(
        objects=[(_block_with_slit(0.3), 1)],
        base=BASE,
        keep=lambda z, t, tool: abs(z - 1.0) < 0.01,
        values=_v("radius", "slice_closing_radius", "0.049", "0.2")),
    # The cap's first layer over a thin stem hangs in the air.
    "unsupported_last": dict(
        objects=[(list(ring(CX, CY, 0, 3, 0, 9, 48)) + list(ring(CX, CY, 0, 8, 8, 11, 72)), 1)],
        base=dict(BASE, wall_loops="3"),
        keep=lambda z, t, tool: abs(z - 8.2) < 0.01,
        values=_v("last", "unsupported_wall_last", "0", "1")),
    "zaa": dict(
        objects=[(list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 3)), 1)],
        base=dict(BASE, zaa_enabled="1", top_shell_layers="3"),
        keep=lambda z, t, tool: z > 2.3,
        values=_v("alt", "zaa_dont_alternate_fill_direction", "0", "1")),

    # --- Strength --------------------------------------------------------------------------------
    "anchor": dict(
        objects=[(list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 3)), 1)],
        base=dict(BASE, sparse_infill_pattern="gyroid", sparse_infill_density="15%"),
        keep=lambda z, t, tool: abs(z - 1.6) < 0.01,
        values=_v("anchor", "infill_anchor", "0", "400%")),
    "lock_depth": dict(
        objects=[(list(box(CX - 10, CY - 10, 0, CX + 10, CY + 10, 6)), 1)],
        base=dict(BASE, sparse_infill_pattern="lockedzag", skin_infill_depth="3", skeleton_infill_density="15%",
                  skin_infill_density="40%"),
        keep=lambda z, t, tool: 2.9 < z < 3.05,
        values=_v("lock", "infill_lock_depth", "0", "2")),
    "solid_template": dict(
        objects=[(list(box(CX - 8, CY - 8, 0, CX + 8, CY + 8, 2)), 1)],
        base=dict(BASE, sparse_infill_density="100%", top_shell_layers="0", bottom_shell_layers="0",
                  internal_solid_infill_pattern="rectilinear"),
        keep=lambda z, t, tool: 0.3 < z < 1.45,
        values=[("none", {}), ("template", {"solid_infill_rotate_template": "0,30,60,90,120,150"})]),
    # A part and its mirror image, side by side; set on the right one only, by slicing it twice.
    "symmetric_a": dict(
        objects=[(list(prism([(CX - 9 + x, CY + y) for x, y in _wing(False)], 0, 3)), 1)],
        base=dict(BASE, sparse_infill_pattern="zigzag", sparse_infill_density="20%", infill_direction="30"),
        keep=lambda z, t, tool: abs(z - 1.6) < 0.01, values=[("off", {})]),
    "symmetric_b": dict(
        objects=[(list(prism([(CX + 9 + x, CY + y) for x, y in _wing(True)], 0, 3)), 1)],
        base=dict(BASE, sparse_infill_pattern="zigzag", sparse_infill_density="20%", infill_direction="30"),
        keep=lambda z, t, tool: abs(z - 1.6) < 0.01,
        values=_v("sym", "symmetric_infill_y_axis", "0", "1")),

    # --- Support ---------------------------------------------------------------------------------
    "independent": dict(
        objects=[(list(ring(CX, CY, 0, 9, 0, 2, 72)) + list(ring(CX, CY, 0, 2.5, 0, 7, 48)) + list(ring(CX, CY, 0, 8, 6, 8, 72)), 1)],
        base=dict(SUPPORT, support_top_z_distance="0.15", support_bottom_z_distance="0.15", layer_height="0.2"),
        keep=lambda z, t, tool: True,
        values=_v("ind", "independent_support_layer_height", "0", "1")),
    # Two piers 5 mm apart and a deck across them: a bridge too short to count as a cantilever.
    "bridge_length": dict(
        objects=[(list(box(CX - 9, CY - 5, 0, CX - 2.5, CY + 5, 6)) + list(box(CX + 2.5, CY - 5, 0, CX + 9, CY + 5, 6))
                  + list(box(CX - 9, CY - 5, 5.8, CX + 9, CY + 5, 8)), 1)],
        base=TREE, keep=_solid, values=_v("len", "max_bridge_length", "0", "10")),
    "critical": dict(
        objects=[(TAIL, 1)], base=TREE, keep=_solid,
        values=_v("crit", "support_critical_regions_only", "0", "1")),
    "loop_interface": dict(
        objects=[(MUSH_LOW, 1)], base=SUPPORT,
        keep=lambda z, t, tool: 7.0 < z < 8.0 and t.startswith("Support"),
        values=_v("loop", "support_interface_loop_pattern", "0", "1")),
    "auto_brim": dict(
        objects=[(MUSH_TALL, 1)], base=SLIM, keep=_solid,
        values=_v("auto", "tree_support_auto_brim", "0", "1", tree_support_brim_width="0")),

    # --- Others ----------------------------------------------------------------------------------
    # A wedge with a corner rounded off by short facets: an ear only where those read as one corner.
    "ears_detect": dict(
        objects=[(list(prism([(CX - 10, CY - 5), (CX + 4, CY - 5)]
                             + [(CX + 4 + 2 * math.sin(math.radians(a)), CY - 3 + 2 * -math.cos(math.radians(a)))
                                for a in range(15, 90, 15)]
                             + [(CX + 6, CY - 3), (CX + 8, CY + 6), (CX - 10, CY + 5)], 0, 3)), 1)],
        base=dict(BASE, brim_type="brim_ears", brim_width="3", brim_ears_max_angle="125"),
        keep=lambda z, t, tool: z < 0.25,
        values=_v("len", "brim_ears_detection_length", "0", "1.5")),
    "exclude": dict(
        objects=[(cylinder(CX - 7, CY + 4, 5, 0, 4), 1), (list(box(CX + 2, CY - 9, 0, CX + 10, CY - 2, 4)), 1),
                 (list(prism([(CX + 3, CY + 3), (CX + 10, CY + 3), (CX + 6.5, CY + 10)], 0, 4)), 1)],
        machine=dict(MACHINE, gcode_flavor="klipper"), base=dict(BASE, exclude_object="1"), exclude=True,
        keep=lambda z, t, tool: abs(z - 0.2) < 0.01,
        values=_v("tol", "exclude_object_tolerance", "0", "1")),
    # Lightning infill is many separate branches: the nozzle travels between them inside the infill. A
    # short minimum travel makes every one of those travels retract unless the setting skips it.
    "infill_retraction": dict(
        objects=[(list(box(CX - 9, CY - 9, 0, CX + 9, CY + 9, 3)), 1)], machine=dict(MACHINE, retraction_minimum_travel=["0.5"]),
        base=dict(BASE, sparse_infill_pattern="lightning", sparse_infill_density="12%"),
        moves=True, keep=lambda z, t, tool: abs(z - 1.6) < 0.01,
        values=_v("reduce", "reduce_infill_retraction", "0", "1")),
    # --- Multimaterial ---------------------------------------------------------------------------
    "prime_volume": dict(
        objects=[(cylinder(CX - 8, CY, 6, 0, 8), 1)], machine=MM_MACHINE, filaments=2, keep=_all,
        base=dict(TOWER, wipe_tower_type="type1"),
        values=_v("vol", "prime_volume", "15", "90")),
    "priming": dict(
        objects=[(cylinder(CX - 8, CY, 6, 0, 4), 1)], machine=MM_MACHINE, filaments=2, start=True,
        keep=lambda z, t, tool: z < 0.25,
        base=dict(TOWER, wipe_tower_type="type2"),
        values=_v("prime", "single_extruder_multi_material_priming", "0", "1")),
}


# --- slicing and reading the G-code --------------------------------------------------------------

_NUM = re.compile(r"([XYZEF])(-?[\d.]+)")
_POLY = re.compile(r"POLYGON=\[(.*)\]")


def parse(gcode, spec):
    """The extrusions as layers: [{"z", "paths": [{"t": type, "f": tool, "w": width, "h": height, "p": [x, y, ...]}]}],
    as settings_process_others_slice.parse reads them. With "moves" a layer also lists its travels ("travel":
    [[x, y, x, y, ...], ...]) and the points where the nozzle retracts ("retract": [x, y, ...]); with "exclude"
    the result has the EXCLUDE_OBJECT outlines too; with "start" the extrusions before the first layer are a
    layer at z = -1."""
    keep, moves = spec["keep"], spec.get("moves", False)
    start = {"z": -1.0, "paths": [], "travel": [], "retract": []}
    layers, cur, path, trav = [], start if spec.get("start") else None, None, None
    x = y = 0.0
    kind, width, tool, height = "", 0.45, 0, 0.2
    outlines = []
    with open(gcode) as f:
        for line in f:
            if line.startswith(";LAYER_CHANGE"):
                cur, path, trav = {"z": None, "paths": [], "travel": [], "retract": []}, None, None
                layers.append(cur)
            elif line.startswith(";Z:") and cur is not None and cur is not start:
                cur["z"] = float(line[3:])
            elif line.startswith(";HEIGHT:"):
                height = float(line[8:])
            elif line.startswith(";TYPE:"):
                kind, path = line[6:].strip(), None
            elif line.startswith(";WIDTH:"):
                width, path = float(line[7:]), None
            elif line.startswith("EXCLUDE_OBJECT_DEFINE"):
                m = _POLY.search(line)
                if m:
                    pts = json.loads("[" + m.group(1) + "]")
                    outlines.append([round(c - (CX if i % 2 == 0 else CY), 3) for pt in pts for i, c in enumerate(pt)])
            elif re.match(r"T\d+\s*$", line):
                tool, path = int(line[1:]), None
            elif line.startswith("G1 ") or line.startswith("G0 "):
                v = dict((k, float(n)) for k, n in _NUM.findall(line.split(";")[0]))
                nx, ny = v.get("X", x), v.get("Y", y)
                moved = (nx, ny) != (x, y)
                e = v.get("E", 0.0)
                if not moved and e < 0.0 and cur is not None and moves:
                    cur["retract"] += [round(x - CX, 3), round(y - CY, 3)]
                if moved and e > 0.0 and cur is not None:
                    trav = None
                    if path is None:
                        path = {"t": kind, "f": tool, "w": round(width, 3), "h": round(height, 3),
                                "p": [round(x - CX, 3), round(y - CY, 3)]}
                        cur["paths"].append(path)
                    path["p"] += [round(nx - CX, 3), round(ny - CY, 3)]
                elif moved:
                    path = None
                    if moves and cur is not None:
                        if trav is None:
                            trav = [round(x - CX, 3), round(y - CY, 3)]
                            cur["travel"].append(trav)
                        trav += [round(nx - CX, 3), round(ny - CY, 3)]
                x, y = nx, ny
    out = []
    for layer in ([start] if spec.get("start") else []) + layers:
        if layer["z"] is None:
            continue
        paths = [dict(p, p=_simplified(p["p"], 0.01)) for p in layer["paths"] if keep(layer["z"], p["t"], p["f"])]
        if paths:
            item = {"z": layer["z"], "paths": paths}
            if moves:
                item["travel"], item["retract"] = layer["travel"], layer["retract"]
            out.append(item)
    return {"layers": out, "outlines": outlines} if spec.get("exclude") else out


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
        # Run in the scratch folder: the CLI leaves a log in the folder it runs in.
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=900, cwd=work)
        gcode = os.path.join(out, "plate_1.gcode")
        if not os.path.exists(gcode):
            raise RuntimeError("%s=%s did not slice:\n%s" % (case, name, (r.stdout + r.stderr)[-2000:]))
        if os.environ.get("KEEP_GCODE"):
            import shutil
            shutil.copy(gcode, os.path.join(os.environ["KEEP_GCODE"], "%s_%s.gcode" % (case, name)))
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
            print(c, v[0], len(layers["layers"] if isinstance(layers, dict) else layers), "layers")
    with gzip.open(DATA, "wt") as f:
        json.dump(data, f, separators=(",", ":"))
    print("wrote", DATA, os.path.getsize(DATA) // 1024, "KB")


if __name__ == "__main__":
    main()
