#!/usr/bin/env python3
"""Expand the first physical co-extrusion tests for the user's 110 mm bed.

Writes files only; never communicates with a printer. Printing tests home XYZ
and level the bed using the user's ssr.gcode startup. Align C manually before
each file. The firmware must support
the explicit Marlin-style command contract described in the test README.
"""

import argparse
import math
from pathlib import Path
import re


TEST_DIR = Path(__file__).resolve().parents[1] / "docs/coextrusion/printer_tests"
LAYER_HEIGHT = 0.2
LINE_WIDTH = 0.45
FILAMENT_DIAMETER = 1.75


def extrusion(length):
    area = LAYER_HEIGHT * (LINE_WIDTH - LAYER_HEIGHT) + math.pi * LAYER_HEIGHT**2 / 4
    return length * area / (math.pi * FILAMENT_DIAMETER**2 / 4)


def render(name, values):
    template = (TEST_DIR / name).read_text(encoding="utf-8")
    # Template-specific instructions are replaced by the concrete file header.
    template = "\n".join(
        line for line in template.splitlines()
        if not line.startswith(("; TEMPLATE:", "; Expand the layer template"))
    )
    return re.sub(r"\{\{([A-Z0-9_]+)\}\}", lambda match: str(values[match[1]]), template) + "\n"


def generate(c_min, c_max):
    if not all(math.isfinite(value) for value in (c_min, c_max)) or c_min >= c_max:
        raise ValueError("C limits must be finite and increasing")
    values = {
        "C_INITIALIZATION_GCODE": "G21\nG90\nM400\nG92 C0 ; ONLY after manually establishing physical C=0",
        "C_FEEDRATE_DEG_MIN": "5400",  # User specified 90 degrees/second.
        "RETRACT_MM": "0.6",
        "RETRACT_FEEDRATE_MM_MIN": "1200",
        "TRAVEL_Z": "5.0",
        "Z_FEEDRATE_MM_MIN": "300",
        "TRAVEL_FEEDRATE_MM_MIN": "1800",
        "PRINT_FEEDRATE_MM_MIN": "900",
        "FIRST_LAYER_Z": "0.2",
        "E_LINE_30MM": f"{extrusion(30):.5f}",
        "E_LINE_20MM": f"{extrusion(20):.5f}",
    }
    for offset in (0, 20, 30, 40, 48):
        values[f"X{offset}"] = f"{25 + offset:.3f}"
    for offset in (0, 8, 20, 30):
        values[f"Y{offset}"] = f"{35 + offset:.3f}"

    # Based on the user's working ssr.gcode: staged heating, G28/G29 and
    # two bed-edge purge lines in absolute E mode. Return retracted by 0.6 mm
    # in relative E mode, matching the test body's first recovery move.
    start = """; Startup adapted from ssr.gcode supplied by the user.
; BEFORE RUN: manually align physical C=0. XYZ homing/leveling are automatic.
; Clear the bed. Confirm extrusion is measured in filament mm, not volume.
G21
G90
M201 X150 Y200 Z300 E800
M203 X250 Y250 Z5 E40
M204 P300 R500 T300
M205 X10.00 Y10.00 Z0.40 E5.00
M205 J0.100
M220 S100
M221 S100
M107
M140 S55
M104 S150
M190 S55
G28
G29
G90
M400
G92 C0 ; Declare the already established physical zero, not an unwind.
M104 S200
M109 S200
G90
M82
G92 E0
G1 Z2 F300
G1 X0 Y10 F3000
G1 Z0.28 F300
G1 Y90 E15 F1200
G1 X0.4 F3000
G1 Y10 E30 F1200
G92 E0
G1 E-0.6 F1200
G1 Z2 F300
M83
; Machine motion settings from ssr.gcode, with C capped at requested 90 deg/s.
M201 X300 Y800 Z50 C8000 E500
M203 X150 Y200 Z5 C90 E25
M204 P300 R500 T300
M205 J0.013
G90
G21
M83
; Entry to test body: already retracted by 0.6 mm."""
    end = """; No additional C reset, homing, or motor release.
G1 Z8.000 F300
G1 X15.000 Y95.000 F1800
M400
M104 S0
M140 S0
M107
; E remains relative and retracted by 0.6 mm; motors remain enabled."""
    values["START_PRINT_GCODE"] = start
    values["END_PRINT_GCODE"] = end

    files = {}
    for stem in ("00_c_direction", "01_fixed_c_four_directions", "02_blue_top_four_directions"):
        files[stem + ".gcode"] = render(stem + ".gcode.in", values)

    wall = ["; Test 03: 20x20x4 mm single-wall square, 20 layers, blue outward faces.",
            start]
    for layer in range(1, 21):
        layer_z = layer * LAYER_HEIGHT
        wall.append(f"; LAYER:{layer} Z={layer_z:.3f}")
        wall.append(render("03_blue_square_wall_layer.gcode.in", {
            **values,
            "LAYER_Z": f"{layer_z:.3f}",
            "LAYER_TRAVEL_Z": f"{layer_z + 2:.3f}",
        }))
    wall.extend(("M400", "G1 C0 F5400", "M400", end))
    files["03_blue_square_wall.gcode"] = "\n".join(wall) + "\n"

    header = f"""; Co-extrusion physical theory test. Generated independently of the slicer.
; Bed 110x110 mm; nozzle 0.4 mm; filament 1.75 mm; layer 0.2; width 0.45.
; Requires a physically verified manual C zero before EACH file.
; Configured physical C limits [{c_min:g}, {c_max:g}] degrees.
; Firmware contract: absolute C degrees; C-only F in degrees/min; M83 filament-mm E.
; Machine mapping: commanded bed Y+ moves toward observer; relative nozzle Y is inverted.
; Color angles and C are physical clockwise-positive from +X, with +Y toward observer.
; No printer communication is performed by the generator.
"""
    # Validate the concrete artifact before writing any file, without building
    # or running slicer tests. Check all commanded coordinates, including purge.
    for name, body in files.items():
        if "{{" in body:
            raise ValueError(f"Unresolved parameter in {name}")
        for line in body.splitlines():
            command = line.split(";", 1)[0]
            if not re.match(r"G(?:0|1|92)\s", command):
                continue
            words = dict((axis, float(value)) for axis, value in
                         re.findall(r"([XYZC])(-?\d+(?:\.\d+)?)", command))
            if "C" in words and not c_min <= words["C"] <= c_max:
                raise ValueError(f"{name}: C{words['C']} exceeds physical limits")
            for axis in "XY":
                if axis in words and not 0 <= words[axis] <= 110:
                    raise ValueError(f"{name}: {axis} exceeds bed bounds")
            if "Z" in words and not 0 <= words["Z"] <= 8:
                raise ValueError(f"{name}: unexpected Z")

    for name, body in files.items():
        (TEST_DIR / name).write_text(header + body, encoding="ascii")
        print(name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--c-min", type=float, required=True, help="Confirmed physical lower C limit")
    parser.add_argument("--c-max", type=float, required=True, help="Confirmed physical upper C limit")
    parser.add_argument("--protocol-confirmed", action="store_true",
                        help="Firmware supports the documented command contract")
    args = parser.parse_args()
    if not args.protocol_confirmed:
        parser.error("Confirm the firmware command contract before generating executable files")
    generate(args.c_min, args.c_max)
