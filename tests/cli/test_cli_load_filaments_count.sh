#!/usr/bin/env bash
# End-to-end check that --load-filaments with more presets than the project defines grows the project
# without disturbing its own filaments or the printer's slot.
#
# A one-filament project is exported from the shipped Bambu Lab H2D presets, where a filament has one
# value per extruder variant. Its per-variant filament_max_volumetric_speed is changed and listed as
# different, and the printer's name is put in its slot of inherits_group, the last one. The project is
# sliced with as many filaments as it has variant entries, the one empty slot per project filament
# refreshing it and the last one a loaded preset: the kept values must survive at their variant
# positions (a loaded count that equals the variant count used to be mistaken for one value per
# filament), and the printer must still hold the last slot, after the new filaments.
#
# usage: test_cli_load_filaments_count.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-load-filaments-count.XXXXXX")"
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
    --load-settings "$PROFILES/machine/Bambu Lab H2D 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL H2D.json" \
    --load-filaments "$PROFILES/filament/Bambu PLA Basic @BBL H2D.json"

# Prints the number of filaments to load: one per variant entry of the project's filament.
LOADED="$("$PY" - "$WORK/base/out.3mf" "$WORK/old.3mf" <<'EOF'
import json, sys, zipfile

src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == "Metadata/project_settings.config":
            config = json.loads(data)
            variants = len(config["filament_self_index"])
            if variants < 2:
                print("SKIP: the project's filament has one variant, so the loaded count cannot equal the variant count", file=sys.stderr)
                sys.exit(77)
            # A different value per variant, none of them the system preset's.
            config["filament_max_volumetric_speed"] = ["%g" % (float(v) + i + 1) for i, v in enumerate(config["filament_max_volumetric_speed"])]
            # One entry for the process, one per filament, one for the printer.
            config["different_settings_to_system"][1] = "filament_max_volumetric_speed"
            config["inherits_group"][-1] = config["printer_settings_id"]
            expected = {key: config[key] for key in ("filament_max_volumetric_speed", "filament_self_index", "inherits_group")}
            with open(dst + ".expected.json", "w") as f:
                json.dump(expected, f)
            print(variants)
            data = json.dumps(config, indent=4)
        zout.writestr(item, data)
EOF
)" || { rc=$?; [ "$rc" = 77 ] && exit 77; exit "$rc"; }

FILAMENTS=""
for ((i = 1; i < LOADED; i++)); do FILAMENTS="$FILAMENTS;"; done
slice grown "$WORK/old.3mf" --load-filaments "$FILAMENTS$PROFILES/filament/Bambu ABS @BBL H2D.json"

"$PY" - "$WORK/grown/out.3mf" "$WORK/old.3mf.expected.json" "$LOADED" <<'EOF' || { tail -n 40 "$WORK/grown/log"; exit 1; }
import json, sys, zipfile

with zipfile.ZipFile(sys.argv[1]) as z:
    config = json.loads(z.read("Metadata/project_settings.config"))
with open(sys.argv[2]) as f:
    expected = json.load(f)
loaded = int(sys.argv[3])
errors = []
# The project's filament keeps its variant entries, at the front of the per-variant lists.
for key in ("filament_max_volumetric_speed", "filament_self_index"):
    want = expected[key]
    if config.get(key, [])[:len(want)] != want:
        errors.append("%s is %r, want it to start with %r" % (key, config.get(key), want))
ids = config.get("filament_settings_id", [])
if len(ids) != loaded or ids[-1] != "Bambu ABS @BBL H2D":
    errors.append("filament_settings_id is %r, want %d entries ending with the loaded preset" % (ids, loaded))
# One entry for the process, one per loaded filament, one for the printer.
inherits = config.get("inherits_group", [])
if len(inherits) != loaded + 2 or inherits[-1] != expected["inherits_group"][-1]:
    errors.append("inherits_group is %r, want %d entries with the printer %r last" % (inherits, loaded + 2, expected["inherits_group"][-1]))
different = config.get("different_settings_to_system", [])
if len(different) != loaded + 2 or "filament_max_volumetric_speed" not in different[1].split(";"):
    errors.append("different_settings_to_system is %r, want %d entries with the kept key in the first filament's" % (different, loaded + 2))
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF
echo "PASS"
