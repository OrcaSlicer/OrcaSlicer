#!/usr/bin/env bash
# End-to-end check that the CLI loads a project's printer and process settings as the GUI does.
#
# The GUI takes every key a project does not list as changed from the project's current system preset:
# keys saved before an option existed, and keys holding an older system value. Keys the project lists
# in different_settings_to_system keep the project's value. A project is exported from the shipped
# Bambu Lab P1S presets; one printer key and one process key are removed, one printer key and one
# process key are changed without being listed, one key is changed and listed, and it is sliced again.
#
# usage: test_cli_project_missing_keys.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-missing-keys.XXXXXX")"
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

# slice <tag> <input> [option...]: slice into $WORK/<tag>/out.3mf with a fresh data directory.
slice() {
    local out="$WORK/$1" input="$2"; shift 2
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$out/datadir" "$@" --slice 0 --outputdir "$out" --export-3mf out.3mf "$input" \
        > "$out/log" 2>&1 || { echo "FAIL: $1: orca-slicer exited $?"; tail -n 40 "$out/log"; exit 1; }
}

slice base "$WORK/cube.stl" \
    --load-settings "$PROFILES/machine/Bambu Lab P1S 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json" \
    --load-filaments "$PROFILES/filament/Bambu PLA Basic @BBL P1S 0.4 nozzle.json"

# The removed keys, with their option defaults from PrintConfig.cpp; stale keys changed without being
# listed as different, which must come back with the system value; and a listed key the project keeps.
"$PY" - "$WORK/base/out.3mf" "$WORK/old.3mf" <<'EOF' || exit $?
import json, sys, zipfile

src, dst = sys.argv[1], sys.argv[2]
missing = {"extruder_clearance_dist_to_rod": "40", "sparse_infill_density": "20%"}
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == "Metadata/project_settings.config":
            config = json.loads(data)
            for key, default in missing.items():
                if config[key] == default:
                    print("SKIP: %s is %s in the system preset, the option default, so the test cannot tell them apart" % (key, default))
                    sys.exit(77)
            expected = {key: config.pop(key) for key in missing}
            for key in ("top_shell_layers", "extruder_clearance_height_to_rod"):
                expected[key] = config[key]
                config[key] = str(int(float(config[key])) + 1)
            expected["wall_loops"] = str(int(config["wall_loops"]) + 1)
            config["wall_loops"] = expected["wall_loops"]
            different = config["different_settings_to_system"]
            different[0] = ";".join([k for k in different[0].split(";") if k] + ["wall_loops"])
            data = json.dumps(config, indent=4)
        zout.writestr(item, data)
with open(dst + ".expected.json", "w") as f:
    json.dump(expected, f)
EOF

slice project "$WORK/old.3mf"

"$PY" - "$WORK/project/out.3mf" "$WORK/old.3mf.expected.json" <<'EOF'
import json, sys, zipfile

with zipfile.ZipFile(sys.argv[1]) as z:
    config = json.loads(z.read("Metadata/project_settings.config"))
with open(sys.argv[2]) as f:
    expected = json.load(f)
errors = ["%s is %r, want %r" % (key, config.get(key), want) for key, want in expected.items() if config.get(key) != want]
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF
status=$?
[ "$status" -eq 0 ] || { tail -n 40 "$WORK/project/log"; exit 1; }
echo "PASS"
