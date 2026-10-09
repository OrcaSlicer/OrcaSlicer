#!/usr/bin/env bash
# End-to-end check of the CLI's arrange rotation policy.
#
# Arrange is translate-only unless --allow-rotations is passed, so a bar that fits the bed only
# diagonally is not placed and the plate ends up with nothing fully inside the print volume. The
# run must then say that rotations were off, on stderr as well as in result.json, because
# result.json is written on Linux only. Passing the flag must place the same bar and slice it, and
# an object that fits axis-aligned must be unaffected by the default.
#
# usage: test_cli_arrange_rotations.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-arrange-rotations.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# box <file> <x> <y> <z>: an ASCII STL box, so the test needs no fixture.
box() {
    "$PY" - "$@" <<'EOF'
import sys

out, sx, sy, sz = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
v = [(x, y, z) for z in (0, sz) for y in (0, sy) for x in (0, sx)]
with open(out, "w") as f:
    f.write("solid box\n")
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d])):
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
    f.write("endsolid box\n")
EOF
}

# Longer than the 256 mm bed edge, shorter than its 362 mm diagonal: it fits only turned.
box "$WORK/bar.stl" 300 10 5
# Fits axis-aligned, so the default must not change anything for it.
box "$WORK/cube.stl" 10 10 10

# slice <tag> <input> [option...] -> $WORK/<tag>/{rc,stdout,stderr,result.json}
slice() {
    local out="$WORK/$1" input="$2"; shift 2
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$out/datadir" \
        --load-settings "$PROFILES/machine/Bambu Lab X1 Carbon 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json" \
        --load-defaultfila "$@" --slice 0 --outputdir "$out" "$input" \
        > "$out/stdout" 2> "$out/stderr"
    echo "$?" > "$out/rc"
}

# exit_code <tag>: the CLI's negative code as the shell saw it, e.g. -50 arrives as 206.
exit_code() { "$PY" -c "import sys; rc = int(open(sys.argv[1]).read()); print(rc - 256 if rc > 127 else rc)" "$WORK/$1/rc"; }

fail() { echo "FAIL: $*"; exit 1; }

# 1. Default: no flag, so no rotations, so the bar does not fit and the reason names rotation.
slice default "$WORK/bar.stl"
[ "$(exit_code default)" = "-50" ] || fail "default: expected -50, got $(exit_code default)"
grep -q "Arrange ran without rotations" "$WORK/default/stderr" \
    || fail "default: stderr does not mention rotations: $(cat "$WORK/default/stderr")"
"$PY" - "$WORK/default/result.json" <<'EOF' || exit 1
import json, sys

result = json.load(open(sys.argv[1]))
if result["return_code"] != -50:
    sys.exit("FAIL: result.json return_code %s, expected -50" % result["return_code"])
if "Arrange ran without rotations" not in result["error_string"]:
    sys.exit("FAIL: result.json error_string does not mention rotations: %s" % result["error_string"])
EOF

# 2. The flag is coBool, so it takes no value: it must place the same bar and slice it.
slice rotations "$WORK/bar.stl" --allow-rotations
[ "$(exit_code rotations)" = "0" ] || fail "--allow-rotations: expected 0, got $(exit_code rotations); $(tail -n 5 "$WORK/rotations/stdout")"
[ -s "$WORK/rotations/plate_1.gcode" ] || fail "--allow-rotations: no g-code written"

# 3. An object that fits axis-aligned is unaffected by the default, and says nothing about rotation.
slice cube "$WORK/cube.stl"
[ "$(exit_code cube)" = "0" ] || fail "cube: expected 0, got $(exit_code cube); $(tail -n 5 "$WORK/cube/stdout")"
[ -s "$WORK/cube/plate_1.gcode" ] || fail "cube: no g-code written"
grep -q "Arrange ran without rotations" "$WORK/cube/stderr" && fail "cube: unexpected rotation message on a successful run"

echo "PASS: arrange rotation default, its failure message, and the opt-in flag"
