"""The toolpaths behind the feature pictures (settings_process_features.py), sliced by OrcaSlicer itself.

Each feature picture shows one kind of line - an outer wall, a bridge, the brim - on a small part chosen
for it, so the cases here are a few simple parts, each sliced once with the orca-slicer CLI. The
extrusions and travel moves of every layer are kept in settings_process_features_toolpaths.json.gz
beside this file, each with its feature type from the G-code (and its filament, where it is not the
first). The pictures are drawn from that file, so drawing them needs no slicer.

    python3 -m gizmo_tips.settings_process_features_slice <path to orca-slicer> [case ...]   # from scripts/
    python3 scripts/gen_gizmo_tips.py settings_process_features

The parts are in millimetres, centred on (100, 100) of a 200 mm bed; the presets are standalone, so
every setting not named here keeps its default from PrintConfig.cpp.
"""

import gzip
import json
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

from .settings_process_slice import CX, CY, PROCESS, _simplified, _write_stl, box, ring
from .settings_process_speed_slice import FILAMENT, MACHINE_MM

OUT = "settings/process"  # shares the folder of settings_process_features.py; draws nothing itself
FRAMES = {}

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "settings_process_features_toolpaths.json.gz")

# Thick layers and few lines, so that each line is seen at the pictures' size.
BASE = dict(PROCESS, **{"layer_height": "0.3", "initial_layer_print_height": "0.3", "wall_loops": "3",
                        "sparse_infill_density": "25%", "sparse_infill_pattern": "grid",
                        "top_shell_layers": "4", "bottom_shell_layers": "3"})


def _leaning(x0, x1, y0, y1, z1, lean):
    """A block whose front face leans out towards -y by `lean` mm over its height: an overhang."""
    v = [(x, y - (lean if (y == y0 and z) else 0.0), z) for z in (0.0, z1) for y in (y0, y1) for x in (x0, x1)]
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        yield v[a], v[b], v[c]
        yield v[a], v[c], v[d]


TOWER = (CX - 16.0, CY - 2.0)  # the prime tower's front left corner, to the left of its part

CASES = {
    # A plain block: its walls, top, ironing, and once cut open, the layers inside.
    "block": dict(model=list(box(CX - 7, CY - 7, 0, CX + 7, CY + 7, 7.2)), settings={"ironing_type": "top", "ironing_spacing": "0.45"}),
    # Two piers and a deck across them.
    "bridge": dict(model=list(box(CX - 9, CY - 5, 0, CX - 5, CY + 5, 5)) + list(box(CX + 5, CY - 5, 0, CX + 9, CY + 5, 5))
                   + list(box(CX - 9, CY - 5, 5, CX + 9, CY + 5, 6.8)), settings={"wall_loops": "2"}),
    # Three narrow bars, each a little too wide for two walls side by side: the classic wall generator
    # fills the gap left between them with gap fill.
    "bars": dict(model=[t for i in range(3) for t in box(CX - 6, CY - 4.2 + 3.0 * i, 0, CX + 6, CY - 2.8 + 3.0 * i, 1.5)],
                 settings={"wall_loops": "1", "wall_generator": "classic"}),
    # A block with its front leaning out at 55 degrees, about the least that prints as an overhang wall
    # with these layers.
    "overhang": dict(model=list(_leaning(CX - 6, CX + 6, CY - 2, CY + 6, 6, 6 * 1.43)),
                     settings={"wall_loops": "2"}),
    # A T: a slab on a stem, held up by supports.
    "support": dict(model=list(box(CX - 2, CY - 2, 0, CX + 2, CY + 2, 6)) + list(box(CX - 8, CY - 7, 6, CX + 8, CY + 7, 7.5)),
                    settings={"wall_loops": "2", "enable_support": "1", "support_type": "normal(auto)",
                              "support_interface_top_layers": "3"}),
    # A round part with its skirt and brim.
    "bed": dict(model=list(ring(CX, CY, 0, 6, 0, 4, 72)),
                settings={"wall_loops": "2", "skirt_loops": "1", "skirt_distance": "3", "brim_type": "outer_only",
                          "brim_width": "3"}),
    # Four small posts: the nozzle travels from one to the next on every layer.
    "travel": dict(model=[t for x, y in ((-6, -6), (6, -6), (6, 6), (-6, 6)) for t in ring(CX + x, CY + y, 0, 2.2, 0, 3, 36)],
                   settings={"wall_loops": "2"}),
    # A block whose walls are the second filament, which puts a prime tower beside it.
    "prime_tower": dict(model=list(box(CX - 5, CY - 5, 0, CX + 5, CY + 5, 6)), filaments=2,
                        settings={"wall_loops": "2", "wall_filament": "2",
                                  "enable_prime_tower": "1", "wipe_tower_type": "type1",
                                  "wipe_tower_wall_type": "rectangle", "prime_tower_width": "8", "prime_volume": "15",
                                  "prime_tower_brim_width": "1", "flush_multiplier": ["0.3"],
                                  "wipe_tower_x": [str(TOWER[0])], "wipe_tower_y": [str(TOWER[1])]}),
}

_NUM = re.compile(r"([XYZEF])(-?[\d.]+)")


def parse(gcode):
    """The layers of the G-code: [{"z", "paths": [{"t": type, "w": width, "p": [x, y, ...], "f": filament}]}],
    a path being a run of extruding moves of one type, width and filament, or of travel moves ("t":
    "Travel"); "f" is left out for the first filament. Wipes, which move while retracting, are left out."""
    layers, cur, path = [], None, None
    x = y = 0.0
    kind, width, tool = "", 0.45, 0
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
            elif re.match(r"T\d+\s*$", line):
                tool, path = int(line[1:]), None
            elif line[:3] in ("G1 ", "G0 ", "G2 ", "G3 "):
                v = dict((k, float(n)) for k, n in _NUM.findall(line.split(";")[0]))
                nx, ny = v.get("X", x), v.get("Y", y)
                e = v.get("E", 0.0)
                if (nx, ny) != (x, y) and cur is not None and e >= 0.0:
                    t, w = (kind, round(width, 3)) if e > 0.0 else ("Travel", 0.0)
                    if path is None or path["t"] != t or path["w"] != w:
                        path = {"t": t, "w": w, "p": [round(x - CX, 3), round(y - CY, 3)]}
                        if tool:
                            path["f"] = tool
                        cur["paths"].append(path)
                    path["p"] += [round(nx - CX, 3), round(ny - CY, 3)]
                elif (nx, ny) != (x, y):
                    path = None
                x, y = nx, ny
    out = []
    for layer in layers:
        paths = [dict(p, p=_simplified(p["p"])) for p in layer["paths"] if len(p["p"]) >= 4]
        if paths and layer["z"] is not None:
            out.append({"z": layer["z"], "paths": paths})
    return out


def slice_case(binary, case):
    spec = CASES[case]
    with tempfile.TemporaryDirectory() as work:
        files = {"machine.json": MACHINE_MM, "process.json": dict(BASE, **spec["settings"]), "filament.json": FILAMENT}
        for name, content in files.items():
            with open(os.path.join(work, name), "w") as f:
                json.dump(content, f)
        stl = os.path.join(work, "model.stl")
        _write_stl(stl, spec["model"])
        out = os.path.join(work, "out")
        filament = os.path.join(work, "filament.json")
        r = subprocess.run([binary, "--datadir", os.path.join(work, "datadir"), "--load-settings",
                            os.path.join(work, "machine.json") + ";" + os.path.join(work, "process.json"),
                            "--load-filaments", ";".join([filament] * spec.get("filaments", 1)),
                            "--arrange", "0", "--slice", "0", "--outputdir", out, stl],
                           capture_output=True, text=True, timeout=900)
        gcode = os.path.join(out, "plate_1.gcode")
        if not os.path.exists(gcode):
            raise RuntimeError("%s did not slice:\n%s" % (case, (r.stdout + r.stderr)[-3000:]))
        return parse(gcode)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    binary = os.path.abspath(sys.argv[1])
    cases = sys.argv[2:] or list(CASES)
    data = {}
    if os.path.exists(DATA):
        with gzip.open(DATA, "rt") as f:
            data = json.load(f)
    with ThreadPoolExecutor(max(1, (os.cpu_count() or 4) // 4)) as pool:
        results = list(pool.map(lambda c: slice_case(binary, c), cases))
    for c, layers in zip(cases, results):
        data[c] = layers
        types = sorted({p["t"] for layer in layers for p in layer["paths"]})
        print(c, len(layers), "layers;", ", ".join(types))
    with gzip.open(DATA, "wt") as f:
        json.dump(data, f, separators=(",", ":"))
    print("wrote", DATA, os.path.getsize(DATA) // 1024, "KB")


if __name__ == "__main__":
    main()
