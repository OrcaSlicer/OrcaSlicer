#!/usr/bin/env bash
# End-to-end check that a per-layer tool change to a later filament is printed when the CLI loads
# filaments without colours.
#
# Filament presets carry no colour, so loading two filaments used to leave filament_colour with one
# entry. Tool ordering sizes its per-filament tables from that list, so the layers switched to filament
# 2 were dropped from the G-code (or the slicer aborted on an out-of-bounds read). A 10 mm cube is
# sliced with two filaments and a switch to filament 2 at 5.0 mm and back at 5.4 mm; every layer must be
# present and the switch to filament 2 must be emitted.
#
# usage: test_cli_filament_colour_count.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-filament-colour.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

"$PY" - "$WORK/cube.stl" <<'EOF'
import sys

v = [(x, y, z) for z in (0, 10) for y in (0, 10) for x in (0, 10)]
with open(sys.argv[1], "w") as f:
    f.write("solid cube\n")
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d])):
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
    f.write("endsolid cube\n")
EOF

cat > "$WORK/changes.json" <<'EOF'
{"mode":"MultiAsSingle","gcodes":[
 {"print_z":5.0,"type":"ToolChange","extruder":2,"color":"#000000","extra":""},
 {"print_z":5.4,"type":"ToolChange","extruder":1,"color":"#FFFFFF","extra":""}]}
EOF

FILAMENT="$PROFILES/filament/Bambu PLA Basic @BBL P1S 0.4 nozzle.json"
timeout 300 "$BIN" --datadir "$WORK/datadir" \
    --load-settings "$PROFILES/machine/Bambu Lab P1S 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json" \
    --load-filaments "$FILAMENT;$FILAMENT" --load-custom-gcodes "$WORK/changes.json" \
    --slice 0 --outputdir "$WORK" --export-3mf out.3mf "$WORK/cube.stl" > "$WORK/log" 2>&1 \
    || { echo "FAIL: orca-slicer exited $?"; tail -n 40 "$WORK/log"; exit 1; }

"$PY" - "$WORK/out.3mf" <<'EOF' || { tail -n 40 "$WORK/log"; exit 1; }
import re, sys, zipfile

with zipfile.ZipFile(sys.argv[1]) as z:
    gcode = z.read("Metadata/plate_1.gcode").decode()
zs = [float(z) for z in re.findall(r"^; Z_HEIGHT: ([0-9.]+)", gcode, re.M)]
gaps = [(a, b) for a, b in zip(zs, zs[1:]) if b - a > 0.2 + 1e-6]
errors = []
if not zs or abs(zs[-1] - 10.0) > 1e-6:
    errors.append("last layer is at %s, want 10.0" % (zs[-1] if zs else None))
if gaps:
    errors.append("layers missing between %s" % gaps)
if not re.search(r"^T1\s*$", gcode, re.M):
    errors.append("no switch to filament 2 (T1)")
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF
echo "PASS"
