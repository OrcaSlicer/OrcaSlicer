"""The toolpaths behind the feature pictures (settings_process_speed.py), sliced by OrcaSlicer itself.

The settings that change one kind of line - its speed, acceleration, jerk, width or flow - show that
kind of line in a picture of one test part, with every other line grey. The part is sliced once here,
with two filaments so that it gets a prime tower, and the extrusions and travel moves of every layer
are kept in settings_process_speed_toolpaths.json.gz beside this file, each with its feature type from
the G-code. The pictures are drawn from that file, so drawing them needs no slicer.

    python3 -m gizmo_tips.settings_process_speed_slice <path to orca-slicer>   # from scripts/
    python3 scripts/gen_gizmo_tips.py settings_process_speed

The part, in millimetres around the centre of the bed: a block with a lower deck bridging over to a
pier, a ledge on supports at the front, a thin fin on top for gap fill, with a skirt and a brim. The
supports' interface is printed with the second filament, which is what puts a prime tower beside it.
"""

import gzip
import json
import os
import re
import subprocess
import sys
import tempfile

from .settings_process_slice import CX, CY, MACHINE, PROCESS, _simplified, _write_stl, box, prism

OUT = "settings/process"  # shares the folder of settings_process_speed.py; draws nothing itself
FRAMES = {}

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "settings_process_speed_toolpaths.json.gz")

TOWER = (81.0, 101.0)  # the prime tower's front left corner on the bed

MODEL = (list(box(CX - 9, CY - 5, 0, CX + 1, CY + 6, 7))  # the block
         + list(box(CX + 6, CY - 5, 0, CX + 9, CY + 6, 5))  # the pier
         + list(box(CX + 0.5, CY - 5, 5, CX + 9, CY + 6, 6.2))  # the deck, bridging from the block to the pier
         + list(box(CX - 8, CY - 9, 3.6, CX - 2, CY - 4.5, 5))  # the ledge, on supports
         + list(prism([(CX - 8.5, CY + 3), (CX - 0.5, CY + 2.1), (CX - 0.5, CY + 3.9)], 6.9, 8.4)))  # the fin

SETTINGS = dict(PROCESS, **{
    "layer_height": "0.3", "initial_layer_print_height": "0.3",
    "wall_loops": "2", "sparse_infill_density": "15%", "top_shell_layers": "3", "bottom_shell_layers": "3",
    # Classic walls leave the fin's narrow end to gap fill, which Arachne would fill with thinner walls.
    "wall_generator": "classic",
    "enable_support": "1", "support_type": "normal(auto)", "bridge_no_support": "1",
    "support_interface_filament": "2",
    "skirt_loops": "1", "skirt_distance": "2", "brim_type": "outer_only", "brim_width": "1.5",
    "ironing_type": "top",
    # A small tower: one rectangle, and as little purged into it as the slicer allows, so that it stands
    # beside the part at about its size rather than dwarfing it.
    "enable_prime_tower": "1", "wipe_tower_type": "type1", "wipe_tower_wall_type": "rectangle",
    "prime_tower_width": "7", "prime_volume": "5", "prime_tower_brim_width": "1", "flush_multiplier": ["0.1"],
    "wipe_tower_x": [str(TOWER[0])], "wipe_tower_y": [str(TOWER[1])],
})
# The CLI needs these of a standalone printer and filament once there is more than one filament. Both
# filaments are one colour, which keeps the purge between them small.
MACHINE_MM = dict(MACHINE, long_retractions_when_cut=["0"], retraction_distances_when_cut=["18"],
                  enable_long_retraction_when_cut="0", nozzle_volume=["0"])
FILAMENT = {"type": "filament", "from": "User", "name": "tips filament", "filament_diameter": ["1.75"],
            "filament_is_support": ["0"], "filament_minimal_purge_on_wipe_tower": ["2"],
            "filament_prime_volume": ["5"], "filament_colour": ["#808080"]}

_NUM = re.compile(r"([XYZEF])(-?[\d.]+)")


def parse(gcode):
    """The layers of the G-code: [{"z", "paths": [{"t": type, "w": width, "p": [x, y, ...]}]}], a path being a
    run of extruding moves of one type and width, or of travel moves ("t": "Travel"). Wipes, which move
    while retracting, are left out."""
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
            elif line.startswith("G1 ") or line.startswith("G0 ") or line.startswith("G2 ") or line.startswith("G3 "):
                v = dict((k, float(n)) for k, n in _NUM.findall(line.split(";")[0]))
                nx, ny = v.get("X", x), v.get("Y", y)
                e = v.get("E", 0.0)
                if (nx, ny) != (x, y) and cur is not None and e >= 0.0:
                    t, w = (kind, round(width, 3)) if e > 0.0 else ("Travel", 0.0)
                    if path is None or path["t"] != t or path["w"] != w:
                        path = {"t": t, "w": w, "p": [round(x - CX, 3), round(y - CY, 3)]}
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


def slice_part(binary):
    with tempfile.TemporaryDirectory() as work:
        files = {"machine.json": MACHINE_MM, "process.json": SETTINGS, "filament.json": FILAMENT}
        for name, content in files.items():
            with open(os.path.join(work, name), "w") as f:
                json.dump(content, f)
        stl = os.path.join(work, "model.stl")
        _write_stl(stl, MODEL)
        out = os.path.join(work, "out")
        filament = os.path.join(work, "filament.json")
        r = subprocess.run([binary, "--datadir", os.path.join(work, "datadir"), "--load-settings",
                            os.path.join(work, "machine.json") + ";" + os.path.join(work, "process.json"),
                            "--load-filaments", filament + ";" + filament,
                            "--arrange", "0", "--slice", "0", "--outputdir", out, stl],
                           capture_output=True, text=True, timeout=900)
        gcode = os.path.join(out, "plate_1.gcode")
        if not os.path.exists(gcode):
            raise RuntimeError("the part did not slice:\n%s" % (r.stdout + r.stderr)[-3000:])
        if len(sys.argv) > 2:  # keep the G-code to look at
            import shutil
            shutil.copy(gcode, sys.argv[2])
        return parse(gcode)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    layers = slice_part(os.path.abspath(sys.argv[1]))
    with gzip.open(DATA, "wt") as f:
        json.dump(layers, f, separators=(",", ":"))
    types = sorted({p["t"] for layer in layers for p in layer["paths"]})
    print(len(layers), "layers;", ", ".join(types))
    print("wrote", DATA, os.path.getsize(DATA) // 1024, "KB")


if __name__ == "__main__":
    main()
