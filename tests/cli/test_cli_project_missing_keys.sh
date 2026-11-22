#!/usr/bin/env bash
# End-to-end check that the CLI loads a project's printer, process and filament settings as the GUI does.
#
# The GUI takes every key a project does not list as changed from the project's current system preset:
# keys saved before an option existed, and keys holding an older system value. Keys the project lists
# in different_settings_to_system keep the project's value. A project is exported from the shipped
# Bambu Lab P1S presets with two filaments; one printer key and one process key are removed, one printer
# key, one process key and two filament keys (one per filament, one per extruder variant) are changed
# without being listed, one process key and the first filament's density are changed and listed, and it
# is sliced again: as is, with --uptodate, and with --load-filaments replacing only the second filament.
# Without any entry in different_settings_to_system the project does not say which values it changed:
# a plain run then keeps every stored value and takes only the removed keys, three filament keys among
# them, from the system presets, while --uptodate still refreshes the unlisted ones.
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
    --load-filaments "$PROFILES/filament/Bambu PLA Basic @BBL P1S 0.4 nozzle.json;$PROFILES/filament/Bambu PLA Basic @BBL P1S 0.4 nozzle.json"

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
            for key in ("filament_cost", "filament_max_volumetric_speed"):
                expected[key] = config[key]
                # Written as the slicer writes them, so the value a project keeps reads back the same.
                config[key] = ["%g" % (float(v) + 1) for v in config[key]]
            expected["wall_loops"] = str(int(config["wall_loops"]) + 1)
            config["wall_loops"] = expected["wall_loops"]
            expected["filament_density"] = [str(float(config["filament_density"][0]) + 1)] + config["filament_density"][1:]
            config["filament_density"] = [expected["filament_density"][0]] + ["%g" % (float(v) + 1) for v in config["filament_density"][1:]]
            # One entry for the process, one per filament, one for the printer.
            different = config["different_settings_to_system"]
            different[0] = ";".join([k for k in different[0].split(";") if k] + ["wall_loops"])
            different[1] = ";".join([k for k in different[1].split(";") if k] + ["filament_density"])
            data = json.dumps(config, indent=4)
        zout.writestr(item, data)
with open(dst + ".expected.json", "w") as f:
    json.dump(expected, f)
EOF

# check <tag> [expected.json]
check() {
    if ! "$PY" - "$WORK/$1/out.3mf" "${2:-$WORK/old.3mf.expected.json}" "$1" <<'EOF'
import json, sys, zipfile

with zipfile.ZipFile(sys.argv[1]) as z:
    config = json.loads(z.read("Metadata/project_settings.config"))
with open(sys.argv[2]) as f:
    expected = json.load(f)
errors = ["%s: %s is %r, want %r" % (sys.argv[3], key, config.get(key), want) for key, want in expected.items() if config.get(key) != want]
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF
    then
        tail -n 40 "$WORK/$1/log"
        exit 1
    fi
}

slice project "$WORK/old.3mf"
check project
slice uptodate "$WORK/old.3mf" --uptodate
check uptodate
# The replaced second filament takes the system values the refresh would have given it.
slice partial "$WORK/old.3mf" --load-filaments ";$PROFILES/filament/Bambu PLA Basic @BBL P1S 0.4 nozzle.json"
check partial

# The same project with no list and with an empty entry for every preset, and without two per-variant
# filament keys and one filament_dev key. A plain run keeps the stored values; --uptodate refreshes them
# all, and the listed keys have no system value to expect.
"$PY" - "$WORK/old.3mf" "$WORK/old.3mf.expected.json" "$WORK" <<'EOF' || exit $?
import json, sys, zipfile

src, expected_path, work = sys.argv[1:]
with open(expected_path) as f:
    expected = json.load(f)
# Read every entry first: writestr() updates the ZipInfo it is given, so zin could not read it again.
with zipfile.ZipFile(src) as zin:
    items = [(item, zin.read(item.filename)) for item in zin.infolist()]
config = json.loads(next(data for item, data in items if item.filename == "Metadata/project_settings.config"))
stored = {key: config.get(key, want) for key, want in expected.items()}
refreshed = {key: want for key, want in expected.items() if key not in ("wall_loops", "filament_density")}
# The removed filament keys, with their option defaults from PrintConfig.cpp.
for key, default in (("nozzle_temperature", "200"), ("filament_flow_ratio", "1"), ("filament_dev_ams_drying_time", "0")):
    if default in config[key]:
        print("SKIP: %s is %s in the system preset, the option default, so the test cannot tell them apart" % (key, default))
        sys.exit(77)
    stored[key] = refreshed[key] = config.pop(key)
for name, different in (("unlisted", None), ("empty_list", [""] * len(config["different_settings_to_system"]))):
    if different is None:
        config.pop("different_settings_to_system")
    else:
        config["different_settings_to_system"] = different
    with zipfile.ZipFile("%s/%s.3mf" % (work, name), "w", zipfile.ZIP_DEFLATED) as zout:
        for item, data in items:
            if item.filename == "Metadata/project_settings.config":
                data = json.dumps(config, indent=4)
            zout.writestr(item, data)
for name, values in (("stored", stored), ("refreshed", refreshed)):
    with open("%s/%s.expected.json" % (work, name), "w") as f:
        json.dump(values, f)
EOF
slice unlisted "$WORK/unlisted.3mf"
check unlisted "$WORK/stored.expected.json"
slice empty_list "$WORK/empty_list.3mf"
check empty_list "$WORK/stored.expected.json"
slice unlisted_uptodate "$WORK/unlisted.3mf" --uptodate
check unlisted_uptodate "$WORK/refreshed.expected.json"
echo "PASS"
