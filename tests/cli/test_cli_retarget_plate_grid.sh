#!/usr/bin/env bash
# End-to-end check that slicing one plate of a project on a bed of another size keeps every object on that
# plate, including one near the plate's edge.
#
# usage: test_cli_retarget_plate_grid.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-retarget-plate-grid.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*"; exit 1; }

"$PY" - "$WORK/cube.stl" <<'EOF'
import sys

v = [(x, y, z) for z in (0, 2) for y in (0, 10) for x in (0, 10)]
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

# run <tag> [option...] -> $WORK/<tag>/log, failing on a non-zero exit
run() {
    local out="$WORK/$1" rc; shift
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$out/datadir" "$@" --outputdir "$out" > "$out/log" 2>&1
    rc=$?
    [ "$rc" = 0 ] || { echo "FAIL: $(basename "$out"): orca-slicer exited $rc"; tail -n 40 "$out/log"; exit 1; }
}

X1C=(--load-settings "$PROFILES/machine/Bambu Lab X1 Carbon 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json"
     --load-filaments "$PROFILES/filament/Bambu PLA Basic @BBL X1C.json")

# retarget <tag> <plate> <machine> <process> <plate 1 x list> <plate 2 x list>: export a two-plate X1 Carbon
#   project with cubes at those distances from each plate's left edge, slice <plate> for the other printer,
#   and expect every cube of that plate in its G-code.
retarget() {
    local tag="$1" plate="$2" machine="$3" process="$4"; shift 4
    "$PY" - "$WORK/cube.stl" "$WORK/$tag.json" "$@" <<'EOF'
import json, sys

stl, out = sys.argv[1], sys.argv[2]
plates = [{"plate_name": "p%d" % (i + 1), "need_arrange": False,
           "objects": [{"path": stl, "count": 1, "filaments": [1], "pos_x": [float(x)], "pos_y": [120]}
                       for x in xs.split(",")]}
          for i, xs in enumerate(sys.argv[3:])]
json.dump({"plates": plates}, open(out, "w"))
EOF
    local cubes
    cubes=$(echo "${@:$plate:1}" | tr ',' '\n' | wc -l)
    run "$tag-project" "${X1C[@]}" --load-assemble-list "$WORK/$tag.json" --export-3mf project.3mf
    run "$tag" --load-settings "$PROFILES/machine/$machine.json;$PROFILES/process/$process.json" \
        --arrange 1 --slice "$plate" "$WORK/$tag-project/project.3mf"
    local gcode="$WORK/$tag/plate_$plate.gcode"
    [ -s "$gcode" ] || fail "$tag: no G-code for plate $plate"
    local objects
    objects=$(grep -o "start printing object, unique label id: [0-9]*" "$gcode" | sort -u | wc -l)
    [ "$objects" -eq "$cubes" ] || fail "$tag: plate $plate printed $objects of its $cubes objects"
}

# Plates on the X1 Carbon's 256 mm bed are 307.2 mm apart, so the cube 20 mm into plate 2 is within the
# H2S bed's 340 mm width of plate 1's origin.
retarget larger 2 "Bambu Lab H2S 0.4 nozzle" "0.20mm Standard @BBL H2S" 120 20,120

# The cube 230 mm into plate 1 is past the A1 mini bed's 180 mm width.
retarget smaller 1 "Bambu Lab A1 mini 0.4 nozzle" "0.20mm Standard @BBL A1M" 150,230 120

echo "PASS: slicing a plate on a larger or smaller bed keeps every object on it"
