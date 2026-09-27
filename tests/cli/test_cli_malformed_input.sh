#!/usr/bin/env bash
# End-to-end checks that malformed CLI input fails cleanly, or loads, instead of crashing the
# orca-slicer binary. Each case lives inline in CLI::run(), so only the binary can reach it.
#
# - A project whose inherits_group does not have one entry per filament plus the process and
#   printer entries is loaded as if it had none.
# - --slice N --arrange 1 on a project without plate metadata slices plate N.
# - An assemble list object with an empty filament list, or a negative filament id, is rejected
#   as a config error.
# - --assemble with no input model is rejected as invalid parameters.
#
# usage: test_cli_malformed_input.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

# From src/libslic3r/Utils.hpp. main() returns them, so the shell sees them modulo 256.
CLI_SUCCESS=0
CLI_INVALID_PARAMS=-2
CLI_CONFIG_FILE_ERROR=-5

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-malformed.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

# Standalone presets: without "inherits" the CLI loads them as-is, with no preset bundle.
cat > "$WORK/machine.json" <<'EOF'
{
    "type": "machine",
    "from": "User",
    "name": "CLI malformed input test printer",
    "printable_area": ["0x0", "200x0", "200x200", "0x200"],
    "printable_height": "100",
    "layer_change_gcode": "G92 E0"
}
EOF
cat > "$WORK/process.json" <<'EOF'
{
    "type": "process",
    "from": "User",
    "name": "CLI malformed input test process"
}
EOF
cat > "$WORK/filament.json" <<'EOF'
{
    "type": "filament",
    "from": "User",
    "name": "CLI malformed input test filament"
}
EOF

"$PY" - "$WORK/cube.stl" <<'EOF'
import sys

v = [(x, y, z) for z in (0, 10) for y in (0, 10) for x in (0, 10)]
with open(sys.argv[1], "w") as f:
    f.write("solid cube\n")
    # Faces wound counter-clockwise seen from outside: -z, +z, -y, +y, -x, +x.
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((a, b, c), (a, c, d)):
            f.write("facet normal 0 0 0\nouter loop\n")
            for i in tri:
                f.write("vertex %g %g %g\n" % v[i])
            f.write("endloop\nendfacet\n")
    f.write("endsolid cube\n")
EOF

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# run <tag> [option...]: run into $WORK/<tag>, keeping the log and the shell status there.
run() {
    local out="$WORK/$1"; shift
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$WORK/datadir" --outputdir "$out" "$@" > "$out/log" 2>&1
    echo $? > "$out/status"
}

# run_presets <tag> [option...]: run with the standalone presets loaded.
run_presets() {
    local tag="$1"; shift
    run "$tag" --load-settings "$WORK/machine.json;$WORK/process.json" --load-filaments "$WORK/filament.json" "$@"
}

# expect_status <tag> <cli code>
expect_status() {
    local got; got="$(cat "$WORK/$1/status")"
    [ "$got" -eq $(( $2 & 255 )) ] || fail "$1: shell status $got, want $(( $2 & 255 )) (code $2)"
}

# expect_result <tag> <return_code>: a failing run must also carry an error_string.
expect_result() {
    "$PY" - "$WORK/$1/result.json" "$2" <<'EOF' || fail "$1: result.json"
import json, sys

try:
    with open(sys.argv[1]) as f:
        result = json.load(f)
except (OSError, ValueError) as e:
    sys.exit("cannot read %s: %s" % (sys.argv[1], e))
want_rc = int(sys.argv[2])
if result.get("return_code") != want_rc:
    sys.exit("return_code %r, want %d" % (result.get("return_code"), want_rc))
if want_rc != 0 and not result.get("error_string"):
    sys.exit("no error_string")
EOF
}

# expect_log <tag> <text>
expect_log() {
    grep -qF -- "$2" "$WORK/$1/log" || fail "$1: log does not mention \"$2\""
}

# expect_gcode <tag>
expect_gcode() {
    compgen -G "$WORK/$1/*.gcode" > /dev/null || fail "$1: no G-code was exported"
}

# rewrite_3mf <in> <out> inherits <json list> | no-plates
rewrite_3mf() {
    "$PY" - "$@" <<'EOF'
import json, re, sys, zipfile

src, dst, mode = sys.argv[1:4]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
    for info in zin.infolist():
        data = zin.read(info.filename)
        if mode == "inherits" and info.filename == "Metadata/project_settings.config":
            config = json.loads(data)
            config["inherits_group"] = json.loads(sys.argv[4])
            data = json.dumps(config, indent=4).encode()
        elif mode == "no-plates":
            if re.match(r"Metadata/plate_\d+\.", info.filename):
                continue
            if info.filename == "Metadata/model_settings.config":
                data = re.sub(rb"\s*<plate>.*?</plate>", b"", data, flags=re.S)
        zout.writestr(info, data)
EOF
}

# assemble_list <file> <filaments json>
assemble_list() {
    cat > "$1" <<EOF
{"plates": [{"plate_name": "p", "need_arrange": false,
             "objects": [{"path": "$WORK/cube.stl", "count": 1, "filaments": $2,
                          "pos_x": [100], "pos_y": [100]}]}]}
EOF
}

echo "== a one-filament project exported by the CLI is the base for the project cases"
run_presets export --slice 0 --export-3mf project.3mf "$WORK/cube.stl"
expect_status export $CLI_SUCCESS
[ -f "$WORK/export/project.3mf" ] || { echo "FAIL: project export failed"; tail -n 40 "$WORK/export/log"; exit 1; }

echo "== an inherits_group of the wrong length is ignored"
for group in '[]' '[""]' '["", "", "", "", ""]'; do
    tag="inherits_$("$PY" -c 'import json, sys; print(len(json.loads(sys.argv[1])))' "$group")"
    rewrite_3mf "$WORK/export/project.3mf" "$WORK/$tag.3mf" inherits "$group"
    run "$tag" --info "$WORK/$tag.3mf"
    expect_status "$tag" $CLI_SUCCESS
    expect_log "$tag" "inherits_group"
done

echo "== --slice 1 --arrange 1 slices a project without plate metadata"
rewrite_3mf "$WORK/export/project.3mf" "$WORK/no_plates.3mf" no-plates
run_presets no_plates --slice 1 --arrange 1 "$WORK/no_plates.3mf"
expect_status no_plates $CLI_SUCCESS
expect_result no_plates $CLI_SUCCESS
expect_gcode no_plates

echo "== an assemble list with a valid filament id slices"
assemble_list "$WORK/assemble_valid.json" '[1]'
run_presets assemble_valid --slice 0 --load-assemble-list "$WORK/assemble_valid.json"
expect_status assemble_valid $CLI_SUCCESS
expect_gcode assemble_valid

echo "== an assemble list with an empty filament list or a negative filament id is rejected"
for filaments in '[]' '[-1]'; do
    if [ "$filaments" = '[]' ]; then tag=assemble_empty; else tag=assemble_negative; fi
    assemble_list "$WORK/$tag.json" "$filaments"
    run_presets "$tag" --slice 0 --load-assemble-list "$WORK/$tag.json"
    expect_status "$tag" $CLI_CONFIG_FILE_ERROR
    expect_result "$tag" $CLI_CONFIG_FILE_ERROR
done

echo "== --assemble with no input model is rejected"
for action in "--slice 0" "--export-3mf out.3mf"; do
    tag="assemble_no_input_${action%% *}"
    tag="${tag//-/}"
    # shellcheck disable=SC2086
    run_presets "$tag" --assemble $action
    expect_status "$tag" $CLI_INVALID_PARAMS
    expect_result "$tag" $CLI_INVALID_PARAMS
    expect_log "$tag" "--assemble"
done

if [ "$fails" -ne 0 ]; then
    for log in "$WORK"/*/log; do
        echo "--- $log"
        tail -n 40 "$log"
    done
    exit 1
fi
echo "PASS"
