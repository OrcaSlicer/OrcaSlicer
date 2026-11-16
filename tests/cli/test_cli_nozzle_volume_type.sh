#!/usr/bin/env bash
# End-to-end check that the CLI slices with, and exports, one nozzle_volume_type per nozzle, as the GUI
# does: the command-line value, else the project's own types, else the printer's defaults. Reopening an
# export whose types are unchanged keeps the flush matrix it carries instead of recomputing it.
#
# usage: test_cli_nozzle_volume_type.sh <orca-slicer binary> <python3> <resources/profiles>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-nozzle-volume-type.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

"$PY" - "$WORK/cube.stl" <<'EOF2'
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
EOF2

# project.py author <src> <dst> [types]: set the flush volume from filament 1 to 2 of every nozzle block to
#   777 and, given a comma-separated list, replace the project's nozzle_volume_type with it.
# project.py check <tag> <3mf> <types> [authored]: the export names these types and, if authored, still
#   carries the 777 of every nozzle block.
cat > "$WORK/project.py" <<'EOF2'
import json, sys, zipfile

def flush_cells(config):
    matrix = config["flush_volumes_matrix"]
    nozzles = len(config["nozzle_diameter"])
    block = len(matrix) // nozzles
    return matrix, [nozzle * block + 1 for nozzle in range(nozzles)]

if sys.argv[1] == "author":
    src, dst = sys.argv[2], sys.argv[3]
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "Metadata/project_settings.config":
                config = json.loads(data)
                matrix, cells = flush_cells(config)
                for cell in cells:
                    matrix[cell] = "777"
                if len(sys.argv) > 4:
                    config["nozzle_volume_type"] = sys.argv[4].split(",")
                data = json.dumps(config, indent=4)
            zout.writestr(item, data)
    sys.exit(0)

tag, path, expected = sys.argv[2], sys.argv[3], sys.argv[4].split(",")
with zipfile.ZipFile(path) as z:
    config = json.loads(z.read("Metadata/project_settings.config"))
errors = []
if config.get("nozzle_volume_type") != expected:
    errors.append("nozzle_volume_type is %s, expected %s" % (config.get("nozzle_volume_type"), expected))
if len(sys.argv) > 5:
    matrix, cells = flush_cells(config)
    for nozzle, cell in enumerate(cells):
        if float(matrix[cell]) != 777:
            errors.append("the authored flush volume of nozzle %d was recomputed: %s" % (nozzle + 1, matrix[cell]))
for e in errors:
    print("FAIL: %s: %s" % (tag, e))
sys.exit(1 if errors else 0)
EOF2

# slice <tag> <input> [option...]: slice into $WORK/<tag>/out.3mf with a fresh data directory.
slice() {
    local out="$WORK/$1" input="$2"; shift 2
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$out/datadir" "$@" --slice 0 --outputdir "$out" --export-3mf out.3mf "$input" \
        > "$out/log" 2>&1 || { echo "FAIL: $1: orca-slicer exited $?"; tail -n 40 "$out/log"; exit 1; }
}

failed=0
check() {
    "$PY" "$WORK/project.py" check "$@" || failed=1
}

BBL="$PROFILES/BBL"
H2D=(--load-settings "$BBL/machine/Bambu Lab H2D 0.4 nozzle.json;$BBL/process/0.20mm Standard @BBL H2D.json"
     --load-filaments "$BBL/filament/Bambu PLA Basic @BBL H2D.json;$BBL/filament/Bambu PLA Basic @BBL H2D.json")

# A two-nozzle printer with two filaments takes the printer's default per nozzle.
slice base "$WORK/cube.stl" "${H2D[@]}"
check base "$WORK/base/out.3mf" "Standard,Standard"

# Printer defaults other than Standard.
PRUSA="$PROFILES/Prusa"
slice defaults "$WORK/cube.stl" \
    --load-settings "$PRUSA/machine/Prusa CORE One INDX 4T HF 0.4 nozzle.json;$PRUSA/process/0.20mm Balanced @Prusa CORE One INDX HF 0.4.json" \
    --load-filaments "$PRUSA/filament/Generic PLA @Prusa CORE One INDX.json;$PRUSA/filament/Generic PLA @Prusa CORE One INDX.json"
check defaults "$WORK/defaults/out.3mf" "High Flow,High Flow,High Flow,High Flow"

# A command-line value naming fewer nozzles than the printer has applies its first entry to the rest.
slice command_line "$WORK/cube.stl" "${H2D[@]}" --nozzle-volume-type "High Flow"
check command_line "$WORK/command_line/out.3mf" "High Flow,High Flow"

# Reopened with an authored flush matrix, a project keeps it as long as its types stay the same: the types
# as exported, High Flow ones, fewer than the nozzles (as from a printer whose defaults name one), or more.
# A nozzle the project's list leaves out takes its first entry.
"$PY" "$WORK/project.py" author "$WORK/base/out.3mf" "$WORK/authored.3mf" || exit 1
"$PY" "$WORK/project.py" author "$WORK/base/out.3mf" "$WORK/high_flow.3mf" "High Flow,High Flow" || exit 1
"$PY" "$WORK/project.py" author "$WORK/base/out.3mf" "$WORK/short.3mf" "Standard" || exit 1
"$PY" "$WORK/project.py" author "$WORK/base/out.3mf" "$WORK/short_high_flow.3mf" "High Flow" || exit 1
"$PY" "$WORK/project.py" author "$WORK/base/out.3mf" "$WORK/long.3mf" "High Flow,High Flow,Standard" || exit 1
for project in authored high_flow short short_high_flow long; do
    slice "$project" "$WORK/$project.3mf"
done
check authored "$WORK/authored/out.3mf" "Standard,Standard" authored
check high_flow "$WORK/high_flow/out.3mf" "High Flow,High Flow" authored
check short "$WORK/short/out.3mf" "Standard,Standard" authored
check short_high_flow "$WORK/short_high_flow/out.3mf" "High Flow,High Flow" authored
check long "$WORK/long/out.3mf" "High Flow,High Flow" authored

[ "$failed" = 0 ] || exit 1
echo "PASS"
