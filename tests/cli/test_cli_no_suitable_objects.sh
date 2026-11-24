#!/usr/bin/env bash
# The reason recorded for CLI_NO_SUITABLE_OBJECTS (-50) has to fit the input the CLI was given.
# Any input format is arranged and sliced, so an object larger than the bed empties the plate just
# as a 3mf with an empty plate does, and the recorded reason must not send such a caller off to
# inspect a 3mf or an upload that is not part of their run.
#
# usage: test_cli_no_suitable_objects.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-no-suitable-objects.XXXXXX")"
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

# Wider than the 256 mm bed and wider than its 362 mm diagonal, so no placement can fit it.
box "$WORK/slab.stl" 400 400 5
# Fits, so the run must succeed: proves the harness itself is sound.
box "$WORK/cube.stl" 10 10 10

# slice <tag> <input> [option...] -> $WORK/<tag>/{rc,stdout,stderr,result.json}
slice() {
    local out="$WORK/$1" input="$2"; shift 2
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$out/datadir" \
        --load-settings "$PROFILES/machine/Bambu Lab X1 Carbon 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json" \
        "$@" --slice 0 --outputdir "$out" "$input" \
        > "$out/stdout" 2> "$out/stderr"
    echo "$?" > "$out/rc"
}

# exit_code <tag>: the CLI's negative code as the shell saw it, e.g. -50 arrives as 206.
exit_code() { "$PY" -c "import sys; rc = int(open(sys.argv[1]).read()); print(rc - 256 if rc > 127 else rc)" "$WORK/$1/rc"; }

fail() { echo "FAIL: $*"; exit 1; }

# --allow-rotations keeps this on the generic reason: a translate-only arrange has its own,
# more specific one. The slab is wider than the 362 mm bed diagonal, so rotation cannot place it.
slice slab "$WORK/slab.stl" --allow-rotations
[ "$(exit_code slab)" = "-50" ] || fail "slab: expected -50, got $(exit_code slab); $(tail -n 5 "$WORK/slab/stdout")"
"$PY" - "$WORK/slab/result.json" <<'EOF' || exit 1
import json, sys

result = json.load(open(sys.argv[1]))
if result["return_code"] != -50:
    sys.exit("FAIL: result.json return_code %s, expected -50" % result["return_code"])
reason = result["error_string"]
if not reason.strip():
    sys.exit("FAIL: result.json carries no error_string")
# The run was handed an STL and asked for g-code: neither a 3mf nor an upload exists to check.
for word in ("3mf", "3MF", "upload"):
    if word in reason:
        sys.exit("FAIL: error_string mentions %r for an STL run: %s" % (word, reason))
EOF

slice cube "$WORK/cube.stl"
[ "$(exit_code cube)" = "0" ] || fail "cube: expected 0, got $(exit_code cube); $(tail -n 5 "$WORK/cube/stdout")"
[ -s "$WORK/cube/plate_1.gcode" ] || fail "cube: no g-code written"

echo "PASS: the no-suitable-objects reason fits a non-3mf run"
