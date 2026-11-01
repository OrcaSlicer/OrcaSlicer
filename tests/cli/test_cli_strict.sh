#!/usr/bin/env bash
# End-to-end check of the CLI --strict option against the real orca-slicer binary.
#
# A model with a large unsupported overhang, sliced with support off, raises the NON_CRITICAL
# "support needed" slicing warning. The CLI lists it in result.json's "warnings" array, and with
# --strict it also fails the run with CLI_SLICING_ERROR. --strict with --no-check is rejected up
# front, because --no-check skips that check.
#
# A preset value the config parser cannot accept is replaced with the option's default and
# reported as a "config_value_substituted" warning. --strict does not escalate that one.
#
# usage: test_cli_strict.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

# From src/libslic3r/Utils.hpp. main() returns them, so the shell sees them modulo 256.
CLI_SUCCESS=0
CLI_INVALID_PARAMS=-2
CLI_SLICING_ERROR=-100

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-strict.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

# Standalone presets: without "inherits" the CLI loads them as-is, with no preset bundle.
cat > "$WORK/machine.json" <<'EOF'
{
    "type": "machine",
    "from": "User",
    "name": "CLI strict test printer",
    "printable_area": ["0x0", "200x0", "200x200", "0x200"],
    "printable_height": "100",
    "layer_change_gcode": "G92 E0"
}
EOF
cat > "$WORK/process.json" <<'EOF'
{
    "type": "process",
    "from": "User",
    "name": "CLI strict test process",
    "enable_support": "0",
    "enforce_support_layers": "0"
}
EOF

# capital.stl is a 40x40mm cap on an 8x8mm stem: the cap reaches ~22mm past the stem, beyond
# the 6mm cantilever limit of PrintObject::is_support_necessary(). cube.stl has no overhang at
# all, so it slices without raising the warning.
"$PY" - "$WORK/capital.stl" "$WORK/cube.stl" <<'EOF'
import sys

def box(x0, y0, z0, x1, y1, z1):
    v = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    # Faces wound counter-clockwise seen from outside: -z, +z, -y, +y, -x, +x.
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        yield v[a], v[b], v[c]
        yield v[a], v[c], v[d]

def write_stl(path, name, triangles):
    with open(path, "w") as f:
        f.write("solid %s\n" % name)
        for tri in triangles:
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
        f.write("endsolid %s\n" % name)

write_stl(sys.argv[1], "capital", (*box(16, 16, 0, 24, 24, 13), *box(0, 0, 12, 40, 40, 14)))
write_stl(sys.argv[2], "cube", box(10, 10, 0, 30, 30, 20))
EOF

# The same process, with an ironing_type the enum does not define. The parser cannot accept
# it and falls back to the option's default, "no ironing".
cat > "$WORK/process_bad_enum.json" <<'EOF'
{
    "type": "process",
    "from": "User",
    "name": "CLI strict test process, invalid ironing_type",
    "enable_support": "0",
    "enforce_support_layers": "0",
    "ironing_type": "every other layer"
}
EOF

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# What run() slices; a case that needs something else assigns to them and restores after.
PROCESS="$WORK/process.json"
MODEL="$WORK/capital.stl"

# run <tag> [option...]: slice into $WORK/<tag>, keeping the log and the shell status there.
run() {
    local out="$WORK/$1"; shift
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$WORK/datadir" --load-settings "$WORK/machine.json;$PROCESS" \
        "$@" --slice 0 --outputdir "$out" "$MODEL" > "$out/log" 2>&1
    echo $? > "$out/status"
}

# expect_status <tag> <cli code>
expect_status() {
    local got; got="$(cat "$WORK/$1/status")"
    [ "$got" -eq $(( $2 & 255 )) ] || fail "$1: shell status $got, want $(( $2 & 255 )) (code $2)"
}

# expect_gcode <tag> yes|no
expect_gcode() {
    if compgen -G "$WORK/$1/*.gcode" > /dev/null; then
        [ "$2" = yes ] || fail "$1: G-code was exported"
    else
        [ "$2" = no ] || fail "$1: no G-code was exported"
    fi
}

# expect_result <tag> <return_code> <strict_mode true|false> <non-critical warning: some|none>
expect_result() {
    "$PY" - "$WORK/$1/result.json" "$2" "$3" "$4" <<'EOF' || fail "$1: result.json"
import json, sys

path, want_rc, want_strict, want_warning = sys.argv[1], int(sys.argv[2]), sys.argv[3] == "true", sys.argv[4]
try:
    with open(path) as f:
        result = json.load(f)
except (OSError, ValueError) as e:
    sys.exit("cannot read %s: %s" % (path, e))

errors = []
if result.get("return_code") != want_rc:
    errors.append("return_code %r, want %d" % (result.get("return_code"), want_rc))
if result.get("strict_mode") is not want_strict:
    errors.append("strict_mode %r, want %r" % (result.get("strict_mode"), want_strict))
warnings = result.get("warnings")
if not isinstance(warnings, list):
    errors.append("warnings %r is not a list" % (warnings,))
else:
    found = any(isinstance(w, dict) and w.get("class") == "slicing_warning_non_critical" for w in warnings)
    if found != (want_warning == "some"):
        errors.append("warnings %r, want %s slicing_warning_non_critical" % (warnings, want_warning))
for e in errors:
    print(e)
sys.exit(1 if errors else 0)
EOF
}

# expect_substitution <tag> <key> <old value> <new value>
expect_substitution() {
    "$PY" - "$WORK/$1/result.json" "$2" "$3" "$4" <<'EOF' || fail "$1: result.json"
import json, sys

path, key, old_value, new_value = sys.argv[1:5]
try:
    with open(path) as f:
        result = json.load(f)
except (OSError, ValueError) as e:
    sys.exit("cannot read %s: %s" % (path, e))

warnings = result.get("warnings")
if not isinstance(warnings, list):
    sys.exit("warnings %r is not a list" % (warnings,))
for w in warnings:
    if isinstance(w, dict) and w.get("class") == "config_value_substituted" and w.get("key") == key:
        if w.get("old_value") == old_value and w.get("new_value") == new_value:
            sys.exit(0)
        sys.exit("%r, want old_value %r and new_value %r" % (w, old_value, new_value))
sys.exit("no config_value_substituted warning for %r in %r" % (key, warnings))
EOF
}

echo "== without --strict the warning is listed and the slice succeeds"
run plain
expect_status plain $CLI_SUCCESS
expect_result plain $CLI_SUCCESS false some
expect_gcode plain yes

echo "== --strict fails the run on the same warning, before G-code export"
run strict --strict
expect_status strict $CLI_SLICING_ERROR
expect_result strict $CLI_SLICING_ERROR true some
expect_gcode strict no

echo "== --strict with --no-check is rejected before slicing"
run conflict --strict --no-check
expect_status conflict $CLI_INVALID_PARAMS
expect_result conflict $CLI_INVALID_PARAMS true none
expect_gcode conflict no
grep -q -- "--strict cannot be combined with --no-check" "$WORK/conflict/log" \
    || fail "conflict: error message missing"

echo "== a value the parser cannot accept is reported, and --strict does not escalate it"
PROCESS="$WORK/process_bad_enum.json"
MODEL="$WORK/cube.stl"
run substituted --strict
PROCESS="$WORK/process.json"
MODEL="$WORK/capital.stl"
expect_status substituted $CLI_SUCCESS
expect_result substituted $CLI_SUCCESS true none
expect_substitution substituted ironing_type "every other layer" "no ironing"
expect_gcode substituted yes
grep -q "ironing_type is not valid" "$WORK/substituted/log" \
    || fail "substituted: console warning missing"

if [ "$fails" -ne 0 ]; then
    for log in "$WORK"/*/log; do
        echo "--- $log"
        tail -n 40 "$log"
    done
    exit 1
fi
echo "PASS"
